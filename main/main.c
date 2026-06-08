/*
 * main.c — Controle Papers, Please
 * APS 2 + Expert (IA + RTOS) | Computação Embarcada — Insper
 *
 * Regras de qualidade aplicadas:
 *   Rule 1.0  — sem erros de cppcheck
 *   Rule 1.1  — variáveis globais somente para ISR→task
 *   Rule 1.2  — variáveis globais de ISR com volatile
 *   Rule 1.3  — somente as modificadas na ISR são globais
 *   Rule 3.0  — sem delay em ISR
 *   Rule 3.2  — sem printf/sprintf em ISR
 *   Rule 3.3  — sem laços em ISR
 *   Rule 4.4  — sem variáveis globais com RTOS (filas/semáforos via struct)
 *
 * NOTA IA: integre a lib do Edge Impulse (edge-impulse-sdk, model-parameters,
 * tflite-model) e mude IA_ENABLED para 1.
 * https://github.com/insper-embarcados/edgeimpulse-runner
 */

/*
 * main.c — Controle Papers, Please
 * APS 2 + Expert (IA + RTOS) | Computação Embarcada — Insper
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#define IA_ENABLED 0

#if IA_ENABLED
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#endif

/* ===== HARDWARE ===== */
#define BTN_APPROVE   13
#define BTN_DENY      15
#define BTN_CLICK     14
#define BTN_INSPECT   12
#define BTN_POWER     11

#define LED_STATUS    17
#define LED_CALIBRADO 16
#define LED_IA_IDLE   20
#define LED_IA_UPDOWN 21
#define LED_IA_WAVE   22

#define I2C_PORT  i2c0
#define I2C_SDA   8
#define I2C_SCL   9
#define MPU_ADDR  0x68

#define MPU_REG_PWR_MGMT  0x6B
#define MPU_REG_ACCEL_CFG 0x1C
#define MPU_REG_GYRO_CFG  0x1B
#define MPU_REG_ACCEL_X   0x3B
#define MPU_REG_GYRO_X    0x43
#define MPU_REG_WHO_AM_I  0x75
#define MPU_WHO_AM_I_VAL  0x68

/* ===== PARAMETROS ===== */
#define CALIB_SAMPLES  200
#define MOUSE_DIVISOR  (32768 / 12)
#define MOUSE_CLAMP    12
#define RATE_LIMIT_HZ  20
#define DEBOUNCE_MS    50
#define IA_WINDOW_SIZE 166   /* = EI_CLASSIFIER_RAW_SAMPLE_COUNT */
#define IA_N_AXES      3

/* ===== TIPOS ===== */
typedef struct {
    uint8_t  pin;
    bool     pressed;
    uint32_t ts_ms;
} btn_event_t;

typedef struct {
    float ax;
    float ay;
    float az;
} imu_sample_t;

typedef struct {
    QueueHandle_t     queue_tx;
    QueueHandle_t     queue_power;
    QueueHandle_t     queue_imu;
    QueueHandle_t     queue_buttons;
    SemaphoreHandle_t sem_rate;
    int32_t           gyro_offsets[3];
} task_params_t;

/* ===== GLOBAIS (somente ISR, volatile) ===== */
static volatile QueueHandle_t     g_isr_queue_buttons;
static volatile SemaphoreHandle_t g_isr_sem_rate;

/* ===== DRIVER MPU6050 ===== */
static void mpu_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_write_blocking(I2C_PORT, MPU_ADDR, buf, 2, false);
}

static void mpu_read(uint8_t reg, uint8_t *buf, uint8_t len) {
    i2c_write_blocking(I2C_PORT, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, MPU_ADDR, buf, len, false);
}

static void mpu_init(void) {
    mpu_write(MPU_REG_PWR_MGMT,  0x00);
    mpu_write(MPU_REG_ACCEL_CFG, 0x00);
    mpu_write(MPU_REG_GYRO_CFG,  0x00);
}

static void mpu_read_accel(int16_t *ax, int16_t *ay, int16_t *az) {
    uint8_t buf[6];
    mpu_read(MPU_REG_ACCEL_X, buf, 6);
    *ax = (int16_t)((buf[0] << 8) | buf[1]);
    *ay = (int16_t)((buf[2] << 8) | buf[3]);
    *az = (int16_t)((buf[4] << 8) | buf[5]);
}

static void mpu_read_gyro(int16_t *gx, int16_t *gy, int16_t *gz) {
    uint8_t buf[6];
    mpu_read(MPU_REG_GYRO_X, buf, 6);
    *gx = (int16_t)((buf[0] << 8) | buf[1]);
    *gy = (int16_t)((buf[2] << 8) | buf[3]);
    *gz = (int16_t)((buf[4] << 8) | buf[5]);
}

/* ===== UTILITARIOS ===== */
static void tx_send(QueueHandle_t queue_tx, const char *s) {
    while (*s != '\0') {
        xQueueSend(queue_tx, s, portMAX_DELAY);
        s++;
    }
}

static int clamp_val(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

/* ===== ISR ===== */
static void btn_callback(uint gpio, uint32_t events) {
    (void)events;
    BaseType_t higher_woken = pdFALSE;

    if (xSemaphoreTakeFromISR(g_isr_sem_rate, &higher_woken) != pdTRUE) {
        return;
    }

    btn_event_t ev;
    ev.pin     = (uint8_t)gpio;
    ev.pressed = (gpio_get(gpio) == 0);
    ev.ts_ms   = to_ms_since_boot(get_absolute_time());

    xQueueSendFromISR((QueueHandle_t)g_isr_queue_buttons, &ev, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}

/* ===== TASKS ===== */
static void init_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    mpu_init();

    uint8_t who = 0;
    mpu_read(MPU_REG_WHO_AM_I, &who, 1);
    printf("[INIT] MPU6050 WHO_AM_I=0x%02X %s\n",
           who, (who == MPU_WHO_AM_I_VAL) ? "OK" : "ERRO");

    printf("[INIT] Calibrando giroscopio — mantenha parado...\n");
    int32_t sx = 0;
    int32_t sy = 0;
    int32_t sz = 0;

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        int16_t gx = 0;
        int16_t gy = 0;
        int16_t gz = 0;
        mpu_read_gyro(&gx, &gy, &gz);
        sx += (int32_t)gx;
        sy += (int32_t)gy;
        sz += (int32_t)gz;
        if ((i % 20) == 0) {
            gpio_put(LED_CALIBRADO, !gpio_get(LED_CALIBRADO));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    p->gyro_offsets[0] = sx / CALIB_SAMPLES;
    p->gyro_offsets[1] = sy / CALIB_SAMPLES;
    p->gyro_offsets[2] = sz / CALIB_SAMPLES;

    gpio_put(LED_CALIBRADO, 1);
    printf("[INIT] Offsets: gx=%d gy=%d gz=%d\n",
           (int)p->gyro_offsets[0],
           (int)p->gyro_offsets[1],
           (int)p->gyro_offsets[2]);

    vTaskDelete(NULL);
}

static void tx_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;
    char c = '\0';

    while (true) {
        xQueueReceive(p->queue_tx, &c, portMAX_DELAY);
        putchar_raw(c);
    }
}

static void power_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;
    bool ligado   = false;
    bool last_btn = true;
    char msg[16];

    while (true) {
        bool cur = (bool)gpio_get(BTN_POWER);

        if (!cur && last_btn) {
            ligado = !ligado;
            gpio_put(LED_STATUS, ligado);
            snprintf(msg, sizeof(msg), "PWR,%d\n", ligado ? 1 : 0);
            tx_send(p->queue_tx, msg);
            xQueueOverwrite(p->queue_power, &ligado);
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        }

        last_btn = cur;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void rate_reset_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;

    while (true) {
        for (int i = 0; i < RATE_LIMIT_HZ; i++) {
            xSemaphoreGive(p->sem_rate);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void imu_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;
    bool ligado = false;
    char msg[24];

    while (!gpio_get(LED_CALIBRADO)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    static imu_sample_t ia_buf[IA_WINDOW_SIZE];
    int ia_idx = 0;

    const TickType_t period    = pdMS_TO_TICKS(10);
    TickType_t       last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, period);

        xQueuePeek(p->queue_power, &ligado, 0);
        if (!ligado) {
            continue;
        }

        int16_t gx = 0;
        int16_t gy = 0;
        int16_t gz = 0;
        mpu_read_gyro(&gx, &gy, &gz);

        int dx = (int)((int32_t)gx - p->gyro_offsets[0]) / MOUSE_DIVISOR;
        int dy = (int)((int32_t)gy - p->gyro_offsets[1]) / MOUSE_DIVISOR;
        dx = clamp_val(dx, -MOUSE_CLAMP, MOUSE_CLAMP);
        dy = clamp_val(dy, -MOUSE_CLAMP, MOUSE_CLAMP);

        if ((dx != 0) || (dy != 0)) {
            snprintf(msg, sizeof(msg), "M,%d,%d\n", dx, dy);
            tx_send(p->queue_tx, msg);
        }

        int16_t ax = 0;
        int16_t ay = 0;
        int16_t az = 0;
        mpu_read_accel(&ax, &ay, &az);

        ia_buf[ia_idx].ax = (float)ax / 16384.0f;
        ia_buf[ia_idx].ay = (float)ay / 16384.0f;
        ia_buf[ia_idx].az = (float)az / 16384.0f;
        ia_idx = (ia_idx + 1) % IA_WINDOW_SIZE;

        if (ia_idx == 0) {
            static imu_sample_t snap[IA_WINDOW_SIZE];
            memcpy(snap, ia_buf, sizeof(ia_buf));
            xQueueSend(p->queue_imu, &snap, 0);
        }
    }
}

static void btn_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;

    static const uint8_t BTN_PINS[4] = {
        BTN_APPROVE, BTN_DENY, BTN_CLICK, BTN_INSPECT
    };

    bool        ligado     = false;
    uint32_t    last_ts[4] = {0u, 0u, 0u, 0u};
    btn_event_t ev;
    char        msg[12];

    while (true) {
        if (xQueueReceive(p->queue_buttons, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xQueuePeek(p->queue_power, &ligado, 0);
        if (!ligado) {
            continue;
        }

        int idx = -1;
        for (int i = 0; i < 4; i++) {
            if (BTN_PINS[i] == ev.pin) {
                idx = i;
                break;
            }
        }

        if (idx < 0) {
            continue;
        }

        if ((ev.ts_ms - last_ts[idx]) < DEBOUNCE_MS) {
            continue;
        }
        last_ts[idx] = ev.ts_ms;

        snprintf(msg, sizeof(msg), "%s,%d\n",
                 ev.pressed ? "BD" : "BU", idx + 1);
        tx_send(p->queue_tx, msg);
    }
}

static void ia_task(void *pvParams) {
    task_params_t *p = (task_params_t *)pvParams;
    static imu_sample_t window[IA_WINDOW_SIZE];
    bool ligado = false;

    while (true) {
        if (xQueueReceive(p->queue_imu, &window, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xQueuePeek(p->queue_power, &ligado, 0);
        if (!ligado) {
            continue;
        }

#if IA_ENABLED
        static float features[IA_WINDOW_SIZE * IA_N_AXES];

        for (int i = 0; i < IA_WINDOW_SIZE; i++) {
            features[(i * IA_N_AXES) + 0] = window[i].ax;
            features[(i * IA_N_AXES) + 1] = window[i].ay;
            features[(i * IA_N_AXES) + 2] = window[i].az;
        }

        signal_t signal;
        numpy::signal_from_buffer(features, IA_WINDOW_SIZE * IA_N_AXES, &signal);

        ei_impulse_result_t result;
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

        if (err != EI_IMPULSE_OK) {
            printf("[IA] Erro na inferencia: %d\n", err);
            continue;
        }

        int   best_idx   = 0;
        float best_score = 0.0f;
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if (result.classification[i].value > best_score) {
                best_score = result.classification[i].value;
                best_idx   = (int)i;
            }
        }

        const char *label = result.classification[best_idx].label;
        printf("[IA] %s (%.2f)\n", label, (double)best_score);

        gpio_put(LED_IA_IDLE,   (strcmp(label, "idle")   == 0) ? 1 : 0);
        gpio_put(LED_IA_UPDOWN, (strcmp(label, "updown") == 0) ? 1 : 0);
        gpio_put(LED_IA_WAVE,   (strcmp(label, "wave")   == 0) ? 1 : 0);
#else
        printf("[IA] stub ax=%.2f ay=%.2f az=%.2f\n",
               (double)window[0].ax,
               (double)window[0].ay,
               (double)window[0].az);
#endif
    }
}

/* ===== MAIN ===== */
int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== Controle Papers, Please - APS2 ===\n");

    static const uint ACTION_BTNS[4] = {
        BTN_APPROVE, BTN_DENY, BTN_CLICK, BTN_INSPECT
    };
    for (int i = 0; i < 4; i++) {
        gpio_init(ACTION_BTNS[i]);
        gpio_set_dir(ACTION_BTNS[i], GPIO_IN);
        gpio_pull_up(ACTION_BTNS[i]);
        gpio_set_irq_enabled_with_callback(ACTION_BTNS[i],
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, btn_callback);
    }

    gpio_init(BTN_POWER);
    gpio_set_dir(BTN_POWER, GPIO_IN);
    gpio_pull_up(BTN_POWER);

    static const uint LEDS[5] = {
        LED_STATUS, LED_CALIBRADO, LED_IA_IDLE, LED_IA_UPDOWN, LED_IA_WAVE
    };
    for (int i = 0; i < 5; i++) {
        gpio_init(LEDS[i]);
        gpio_set_dir(LEDS[i], GPIO_OUT);
        gpio_put(LEDS[i], 0);
    }

    static task_params_t params;
    params.gyro_offsets[0] = 0;
    params.gyro_offsets[1] = 0;
    params.gyro_offsets[2] = 0;

    params.queue_buttons = xQueueCreate(16,  sizeof(btn_event_t));
    params.queue_tx      = xQueueCreate(256, sizeof(char));
    params.queue_power   = xQueueCreate(1,   sizeof(bool));
    params.queue_imu     = xQueueCreate(2,   IA_WINDOW_SIZE * sizeof(imu_sample_t));
    params.sem_rate      = xSemaphoreCreateCounting(RATE_LIMIT_HZ, RATE_LIMIT_HZ);

    g_isr_queue_buttons = params.queue_buttons;
    g_isr_sem_rate      = params.sem_rate;

    bool off = false;
    xQueueSend(params.queue_power, &off, 0);

    xTaskCreate(init_task,       "init",       2048, &params, 4, NULL);
    xTaskCreate(tx_task,         "tx",          512, &params, 3, NULL);
    xTaskCreate(power_task,      "power",       512, &params, 2, NULL);
    xTaskCreate(rate_reset_task, "rate_reset",  256, &params, 2, NULL);
    xTaskCreate(imu_task,        "imu",        1024, &params, 1, NULL);
    xTaskCreate(ia_task,         "ia",         4096, &params, 1, NULL);
    xTaskCreate(btn_task,        "btn",         512, &params, 1, NULL);

    vTaskStartScheduler();

    while (true) {}
    return 0;
}