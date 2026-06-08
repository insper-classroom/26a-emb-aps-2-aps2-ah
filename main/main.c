/*
 * main.c — Passo 6 + IA (LED + serial)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Funcoes da IA (definidas em ia.cpp) */
extern int ia_classificar(float *features, int n, float *confianca_out, const char **label_out);
extern int ia_window_size(void);

#define LED_STATUS    16
#define LED_IA_IDLE   18
#define LED_IA_UPDOWN 19
#define LED_IA_WAVE   20

#define BTN_APPROVE   13
#define BTN_DENY      15
#define BTN_CLICK     14
#define BTN_INSPECT   12
#define BTN_POWER     11

#define I2C_PORT      i2c0
#define I2C_SDA       8
#define I2C_SCL       9
#define MPU_ADDR      0x68

#define DEBOUNCE_MS   50
#define IA_WINDOW     166   /* = EI_CLASSIFIER_RAW_SAMPLE_COUNT */

const uint action_pins[] = {BTN_APPROVE, BTN_DENY, BTN_CLICK, BTN_INSPECT};
#define NUM_ACTIONS 4

QueueHandle_t xQueueTX;
QueueHandle_t xQueueButtons;
QueueHandle_t xQueuePower;
QueueHandle_t xQueueIMU;   /* janela de IA pronta -> ia_task */

typedef struct {
    uint8_t  pin;
    bool     pressed;
    uint32_t ts_ms;
} btn_event_t;

/* Buffer de uma janela: 166*3 floats */
typedef struct {
    float data[IA_WINDOW * 3];
} ia_window_t;

void tx_send(const char *s) {
    while (*s != '\0') {
        xQueueSend(xQueueTX, s, portMAX_DELAY);
        s++;
    }
}

bool power_is_on(void) {
    bool on = false;
    xQueuePeek(xQueuePower, &on, 0);
    return on;
}

/* ===== MPU6050 ===== */
void mpu6050_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    i2c_write_blocking(I2C_PORT, MPU_ADDR, buf, 2, false);
}

void mpu6050_read(uint8_t reg, uint8_t *buf, uint8_t len) {
    i2c_write_blocking(I2C_PORT, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, MPU_ADDR, buf, len, false);
}

/* ===== ISR botoes ===== */
void btn_callback(uint gpio, uint32_t events) {
    (void)events;
    btn_event_t ev;
    ev.pin     = (uint8_t)gpio;
    ev.pressed = (gpio_get(gpio) == 0);
    ev.ts_ms   = to_ms_since_boot(get_absolute_time());

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(xQueueButtons, &ev, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ===== Tasks ===== */
void tx_task(void *params) {
    char c;
    while (true) {
        if (xQueueReceive(xQueueTX, &c, portMAX_DELAY) == pdTRUE) {
            putchar_raw(c);
        }
    }
}

void power_task(void *params) {
    gpio_init(LED_STATUS);
    gpio_set_dir(LED_STATUS, GPIO_OUT);
    gpio_put(LED_STATUS, 0);

    gpio_init(BTN_POWER);
    gpio_set_dir(BTN_POWER, GPIO_IN);
    gpio_pull_up(BTN_POWER);

    bool ligado   = false;
    bool last_btn = true;

    while (true) {
        bool cur = gpio_get(BTN_POWER);
        if (!cur && last_btn) {
            ligado = !ligado;
            gpio_put(LED_STATUS, ligado);
            xQueueOverwrite(xQueuePower, &ligado);
            char msg[12];
            snprintf(msg, sizeof(msg), "PWR,%d\n", ligado ? 1 : 0);
            tx_send(msg);
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        }
        last_btn = cur;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void btn_task(void *params) {
    btn_event_t ev;
    uint32_t last_ts[NUM_ACTIONS] = {0, 0, 0, 0};

    while (true) {
        if (xQueueReceive(xQueueButtons, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (!power_is_on()) continue;

        int idx = -1;
        for (int i = 0; i < NUM_ACTIONS; i++) {
            if (action_pins[i] == ev.pin) { idx = i; break; }
        }
        if (idx < 0) continue;
        if ((ev.ts_ms - last_ts[idx]) < DEBOUNCE_MS) continue;
        last_ts[idx] = ev.ts_ms;

        char msg[12];
        snprintf(msg, sizeof(msg), "%s,%d\n", ev.pressed ? "BD" : "BU", idx + 1);
        tx_send(msg);
    }
}

void imu_task(void *params) {
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    mpu6050_write(0x6B, 0x00);

    uint8_t who = 0;
    mpu6050_read(0x75, &who, 1);
    printf("[MPU6050] WHO_AM_I = 0x%02X %s\n", who, (who == 0x68) ? "OK" : "ERRO");

    static ia_window_t win;
    int idx = 0;

    while (true) {
        if (!power_is_on()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Acelerometro (pra IA) */
        uint8_t a[6];
        mpu6050_read(0x3B, a, 6);
        int16_t ax = (int16_t)((a[0] << 8) | a[1]);
        int16_t ay = (int16_t)((a[2] << 8) | a[3]);
        int16_t az = (int16_t)((a[4] << 8) | a[5]);

        /* Giroscopio (pro mouse) */
        uint8_t g[6];
        mpu6050_read(0x43, g, 6);
        int16_t gx = (int16_t)((g[0] << 8) | g[1]);
        int16_t gy = (int16_t)((g[2] << 8) | g[3]);

        int dx = gx / 2730;
        int dy = gy / 2730;
        if (dx > 12)  dx = 12;
        if (dx < -12) dx = -12;
        if (dy > 12)  dy = 12;
        if (dy < -12) dy = -12;
        if (dx > -2 && dx < 2) dx = 0;
        if (dy > -2 && dy < 2) dy = 0;
        if (dx != 0 || dy != 0) {
            char msg[16];
            snprintf(msg, sizeof(msg), "M,%d,%d\n", dx, dy);
            tx_send(msg);
        }

        /* Alimenta o buffer da IA */
        win.data[idx * 3 + 0] = (float)ax;
        win.data[idx * 3 + 1] = (float)ay;
        win.data[idx * 3 + 2] = (float)az;
        idx++;

        if (idx >= IA_WINDOW) {
            idx = 0;
            xQueueSend(xQueueIMU, &win, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ia_task(void *params) {
    gpio_init(LED_IA_IDLE);   gpio_set_dir(LED_IA_IDLE,   GPIO_OUT);
    gpio_init(LED_IA_UPDOWN); gpio_set_dir(LED_IA_UPDOWN, GPIO_OUT);
    gpio_init(LED_IA_WAVE);   gpio_set_dir(LED_IA_WAVE,   GPIO_OUT);

    static ia_window_t win;

    while (true) {
        if (xQueueReceive(xQueueIMU, &win, portMAX_DELAY) != pdTRUE) continue;

        float conf = 0.0f;
        const char *label = "?";
        int gesto = ia_classificar(win.data, IA_WINDOW * 3, &conf, &label);

        if (gesto < 0) {
            printf("[IA] erro\n");
            continue;
        }

        printf("[IA] %s (%.2f)\n", label, (double)conf);

        gpio_put(LED_IA_IDLE,   strcmp(label, "idle")   == 0);
        gpio_put(LED_IA_UPDOWN, strcmp(label, "updown") == 0);
        gpio_put(LED_IA_WAVE,   strcmp(label, "wave")   == 0);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== Controle + IA (LED) ===\n");

    for (int i = 0; i < NUM_ACTIONS; i++) {
        gpio_init(action_pins[i]);
        gpio_set_dir(action_pins[i], GPIO_IN);
        gpio_pull_up(action_pins[i]);
    }
    gpio_set_irq_enabled_with_callback(action_pins[0],
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, btn_callback);
    for (int i = 1; i < NUM_ACTIONS; i++) {
        gpio_set_irq_enabled(action_pins[i],
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    }

    xQueueTX      = xQueueCreate(256, sizeof(char));
    xQueueButtons = xQueueCreate(16,  sizeof(btn_event_t));
    xQueuePower   = xQueueCreate(1,   sizeof(bool));
    xQueueIMU     = xQueueCreate(2,   sizeof(ia_window_t));

    bool off = false;
    xQueueOverwrite(xQueuePower, &off);

    xTaskCreate(tx_task,    "tx",    512,  NULL, 3, NULL);
    xTaskCreate(power_task, "power", 512,  NULL, 2, NULL);
    xTaskCreate(btn_task,   "btn",   512,  NULL, 1, NULL);
    xTaskCreate(imu_task,   "imu",   1024, NULL, 1, NULL);
    xTaskCreate(ia_task,    "ia",    4096, NULL, 1, NULL);

    vTaskStartScheduler();
    while (true) {}
    return 0;
}