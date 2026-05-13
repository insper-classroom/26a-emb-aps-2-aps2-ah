/*
 * main.c — Controle Papers, Please com Bluetooth HC-06
 * APS 2 + Expert Bluetooth+RTOS - Computação Embarcada
 *
 * Arquitetura:
 *   imu_task       — lê MPU6050 e envia "M,dx,dy\n" via fila → HC-06
 *   btn_task       — consome fila de botões, envia "BD,n / BU,n\n" via HC-06
 *   tx_task        — consome xQueueTX e escreve bytes na UART do HC-06
 *   status_task    — monitora pino STATE do HC-06 e controla LED
 *   power_task     — botão liga/desliga, notifica tasks via xQueuePower
 *
 *   btn_callback (ISR) — enfileira eventos brutos em xQueueButtons
 *   uart_rx_handler(ISR) — recebe bytes do HC-06 (reservado para extensões)
 *
 * Protocolo serial (lido pelo controller.py no PC via COM Bluetooth):
 *   M,dx,dy    → movimento do mouse
 *   BD,n       → botão n pressionado
 *   BU,n       → botão n solto
 *     1=APPROVE(tecla A), 2=DENY(tecla X), 3=CLICK(mouse esq), 4=INSPECT(tecla I)
 *   PWR,1/0    → controle ligado/desligado
 *
 * Hardware HC-06:
 *   HC-06 STATE → GP3   (indica conexão BT: HIGH=conectado)
 *   HC-06 RXD   → GP4   (TX1 da Pico)
 *   HC-06 TXD   → GP5   (RX1 da Pico)
 *   HC-06 ENABLE→ GP6
 *   HC-06 VCC   → VBUS (5V)
 *   HC-06 GND   → GND
 *
 * Regras de qualidade:
 *   Rule 1.1/1.2/1.3 — Zero globais
 *   Rule 3.0-3.3      — ISR curta: sem delay, printf, for, display
 *   Rule 4.1-4.4      — FreeRTOS correto, sem globais de estado
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ── UART HC-06 ─────────────────────────────────────────────── */
#define HC06_UART_ID    uart1
#define HC06_BAUD_RATE  9600
#define HC06_TX_PIN     4     /* GP4 = UART1 TX */
#define HC06_RX_PIN     5     /* GP5 = UART1 RX */
#define HC06_EN_PIN     6
#define HC06_STATE_PIN  3     /* HIGH = BT conectado */
#define HC06_NAME       "PapersPlease-Ctrl"
#define HC06_PIN_BT     "1234"

/* ── IMU ────────────────────────────────────────────────────── */
#define I2C_PORT            i2c0
#define I2C_SDA_PIN         8
#define I2C_SCL_PIN         9
#define MPU6050_ADDR        0x68
#define MPU6050_PWR_MGMT_1  0x6B
#define MPU6050_GYRO_XOUT_H 0x43
#define GYRO_DEADZONE       10
#define MAX_MOUSE_SPEED     12
#define CALIBRATION_SAMPLES 3000

/* ── BOTÕES ─────────────────────────────────────────────────── */
#define BTN_APPROVE_PIN  16
#define BTN_DENY_PIN     17
#define BTN_CLICK_PIN    18
#define BTN_INSPECT_PIN  19
#define BTN_POWER_PIN    20
#define LED_STATUS_PIN   25
#define NUM_BTNS         4

/* ── ANTI-CHEAT ─────────────────────────────────────────────── */
#define DEBOUNCE_MS          50
#define MAX_BTN_EVENTS_PER_S 20
#define RATE_LIMIT_PERIOD_MS 1000

/* ── TIPOS ──────────────────────────────────────────────────── */
typedef struct {
    uint8_t  pin;
    uint8_t  state;
    uint32_t time_ms;
} button_event_t;

typedef enum { PWR_ON, PWR_OFF } power_event_t;

/* ── RECURSOS RTOS ──────────────────────────────────────────── */
static QueueHandle_t     xQueueButtons;  /* ISR → btn_task        */
static QueueHandle_t     xQueueTX;       /* tasks → tx_task       */
static QueueHandle_t     xQueuePower;    /* power_task → outras   */
static SemaphoreHandle_t xRateSem;       /* anti-cheat rate limit */

/* ── HELPERS ────────────────────────────────────────────────── */
static inline int pin_to_idx(uint8_t pin) {
    switch (pin) {
        case BTN_APPROVE_PIN: return 0;
        case BTN_DENY_PIN:    return 1;
        case BTN_CLICK_PIN:   return 2;
        case BTN_INSPECT_PIN: return 3;
        default:              return -1;
    }
}

/* ── HC-06 CONFIG (comandos AT) ─────────────────────────────── */
static void hc06_send_at(const char *cmd) {
    uart_puts(HC06_UART_ID, cmd);
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void hc06_config(void) {
    /* Coloca em modo AT: EN=HIGH, aguarda */
    gpio_put(HC06_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    hc06_send_at("AT");                              /* ping            */
    hc06_send_at("AT+BAUD4");                        /* 9600 baud       */
    hc06_send_at("AT+NAME" HC06_NAME);               /* nome BT         */
    hc06_send_at("AT+PIN" HC06_PIN_BT);              /* PIN de pareamento */

    /* Volta modo normal */
    gpio_put(HC06_EN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* ── MPU6050 ────────────────────────────────────────────────── */
static void mpu6050_init(void) {
    uint8_t buf[2] = {MPU6050_PWR_MGMT_1, 0x00};
    i2c_write_blocking(I2C_PORT, MPU6050_ADDR, buf, 2, false);
}

static void mpu6050_read_gyro(int16_t gyro[3]) {
    uint8_t buffer[6];
    uint8_t reg = MPU6050_GYRO_XOUT_H;
    i2c_write_blocking(I2C_PORT, MPU6050_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, MPU6050_ADDR, buffer, 6, false);
    gyro[0] = (int16_t)((buffer[0] << 8) | buffer[1]);
    gyro[1] = (int16_t)((buffer[2] << 8) | buffer[3]);
    gyro[2] = (int16_t)((buffer[4] << 8) | buffer[5]);
}

/* ── ISR: UART RX do HC-06 (reservado para extensões) ──────── */
static void uart_rx_handler(void) {
    /* Descarta bytes recebidos por enquanto */
    while (uart_is_readable(HC06_UART_ID)) {
        uart_getc(HC06_UART_ID);
    }
}

/* ── ISR: botões (Rule 3.0-3.3, Rule 4.1) ──────────────────── */
static void btn_callback(uint gpio, uint32_t events) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Rate limiting via semáforo — Rule 4.1 */
    if (xSemaphoreTakeFromISR(xRateSem, &xHigherPriorityTaskWoken) == pdFALSE) {
        return;
    }

    button_event_t event;
    event.pin     = (uint8_t)gpio;
    event.state   = (events & GPIO_IRQ_EDGE_FALL) ? 1U : 0U;
    event.time_ms = to_ms_since_boot(get_absolute_time());

    xQueueSendFromISR(xQueueButtons, &event, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* ── TASK: TX — envia bytes da fila para HC-06 via UART ─────── */
static void tx_task(void *pvParameters) {
    (void)pvParameters;
    uint8_t byte;
    while (1) {
        if (xQueueReceive(xQueueTX, &byte, portMAX_DELAY) == pdPASS) {
            uart_putc_raw(HC06_UART_ID, (char)byte);
        }
    }
}

/* ── HELPER: envia string para xQueueTX byte a byte ─────────── */
static void bt_send(const char *str) {
    while (*str) {
        uint8_t b = (uint8_t)(*str++);
        xQueueSend(xQueueTX, &b, portMAX_DELAY);
    }
}

/* ── TASK: Rate limiter — recarrega semáforo a cada 1s ──────── */
static void rate_reset_task(void *pvParameters) {
    (void)pvParameters;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RATE_LIMIT_PERIOD_MS));
        for (int i = 0; i < MAX_BTN_EVENTS_PER_S; i++) {
            xSemaphoreGive(xRateSem);
        }
    }
}

/* ── TASK: IMU → BT ─────────────────────────────────────────── */
static void imu_task(void *pvParameters) {
    (void)pvParameters;

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    mpu6050_init();

    /* Calibração igual ao Enzo */
    int32_t gyro_x_offset = 0;
    int32_t gyro_y_offset = 0;
    int16_t gyro[3];

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        mpu6050_read_gyro(gyro);
        gyro_x_offset += gyro[0];
        gyro_y_offset += gyro[1];
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    gyro_x_offset /= CALIBRATION_SAMPLES;
    gyro_y_offset /= CALIBRATION_SAMPLES;

    bool controller_on = false;
    power_event_t pwr_event;
    char buf[32];

    while (1) {
        if (xQueueReceive(xQueuePower, &pwr_event, 0) == pdPASS) {
            controller_on = (pwr_event == PWR_ON);
        }

        if (!controller_on) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        mpu6050_read_gyro(gyro);

        int16_t corrected_gx = gyro[0] - (int16_t)gyro_x_offset;
        int16_t corrected_gy = gyro[1] - (int16_t)gyro_y_offset;

        int16_t mouse_dx = -corrected_gy / 100;
        int16_t mouse_dy = -corrected_gx / 100;

        if (abs(corrected_gy) < GYRO_DEADZONE) mouse_dx = 0;
        if (abs(corrected_gx) < GYRO_DEADZONE) mouse_dy = 0;

        /* Clamp anti-cheat */
        if (mouse_dx >  MAX_MOUSE_SPEED) mouse_dx =  MAX_MOUSE_SPEED;
        if (mouse_dx < -MAX_MOUSE_SPEED) mouse_dx = -MAX_MOUSE_SPEED;
        if (mouse_dy >  MAX_MOUSE_SPEED) mouse_dy =  MAX_MOUSE_SPEED;
        if (mouse_dy < -MAX_MOUSE_SPEED) mouse_dy = -MAX_MOUSE_SPEED;

        if (mouse_dx != 0 || mouse_dy != 0) {
            snprintf(buf, sizeof(buf), "M,%d,%d\n", mouse_dx, -mouse_dy);
            bt_send(buf);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── TASK: Botões → BT ──────────────────────────────────────── */
static void btn_task(void *pvParameters) {
    (void)pvParameters;

    const uint8_t BTN_PINS[NUM_BTNS] = {
        BTN_APPROVE_PIN, BTN_DENY_PIN,
        BTN_CLICK_PIN,   BTN_INSPECT_PIN
    };

    for (int i = 0; i < NUM_BTNS; i++) {
        gpio_init(BTN_PINS[i]);
        gpio_set_dir(BTN_PINS[i], GPIO_IN);
        gpio_pull_up(BTN_PINS[i]);
        gpio_set_irq_enabled_with_callback(
            BTN_PINS[i],
            GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
            true, &btn_callback
        );
    }

    /* Timestamps de debounce locais — sem global, Rule 4.4 */
    uint32_t last_event_ms[NUM_BTNS] = {0U, 0U, 0U, 0U};

    bool controller_on = false;
    button_event_t ev;
    power_event_t pwr_event;
    char buf[16];

    while (1) {
        if (xQueueReceive(xQueuePower, &pwr_event, 0) == pdPASS) {
            controller_on = (pwr_event == PWR_ON);
        }

        if (xQueueReceive(xQueueButtons, &ev, pdMS_TO_TICKS(10)) != pdPASS) {
            continue;
        }

        /* Debounce na task — sem global */
        int idx = pin_to_idx(ev.pin);
        if (idx < 0) continue;
        if ((ev.time_ms - last_event_ms[idx]) < DEBOUNCE_MS) continue;
        last_event_ms[idx] = ev.time_ms;

        if (!controller_on) continue;

        uint8_t btn_id = 0;
        switch (ev.pin) {
            case BTN_APPROVE_PIN: btn_id = 1; break;
            case BTN_DENY_PIN:    btn_id = 2; break;
            case BTN_CLICK_PIN:   btn_id = 3; break;
            case BTN_INSPECT_PIN: btn_id = 4; break;
            default: break;
        }

        if (btn_id != 0) {
            snprintf(buf, sizeof(buf),
                     ev.state ? "BD,%d\n" : "BU,%d\n", btn_id);
            bt_send(buf);
        }
    }
}

/* ── TASK: Power + LED controlado pelo STATE do HC-06 ───────── */
static void power_task(void *pvParameters) {
    (void)pvParameters;

    gpio_init(BTN_POWER_PIN);
    gpio_set_dir(BTN_POWER_PIN, GPIO_IN);
    gpio_pull_up(BTN_POWER_PIN);

    /* LED controlado pelo STATE do HC-06 */
    gpio_init(LED_STATUS_PIN);
    gpio_set_dir(LED_STATUS_PIN, GPIO_OUT);
    gpio_put(LED_STATUS_PIN, 0);

    /* Pino STATE do HC-06 como entrada */
    gpio_init(HC06_STATE_PIN);
    gpio_set_dir(HC06_STATE_PIN, GPIO_IN);

    bool last_btn_state = true;
    bool controller_on  = false;
    TickType_t last_press = 0;
    char buf[16];

    while (1) {
        /* LED = STATE do HC-06 (HIGH = BT conectado) */
        gpio_put(LED_STATUS_PIN, gpio_get(HC06_STATE_PIN) ? 1 : 0);

        /* Botão power com debounce */
        bool current = gpio_get(BTN_POWER_PIN);
        if (!current && last_btn_state) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_press) > pdMS_TO_TICKS(300)) {
                controller_on = !controller_on;

                power_event_t pwr = controller_on ? PWR_ON : PWR_OFF;
                xQueueOverwrite(xQueuePower, &pwr);

                snprintf(buf, sizeof(buf), "PWR,%d\n", controller_on ? 1 : 0);
                bt_send(buf);

                last_press = now;
            }
        }
        last_btn_state = current;

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ── TASK: Inicialização do HC-06 ───────────────────────────── */
static void init_task(void *pvParameters) {
    (void)pvParameters;

    /* Configura UART do HC-06 */
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);
    gpio_set_function(HC06_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(HC06_RX_PIN, GPIO_FUNC_UART);

    gpio_init(HC06_EN_PIN);
    gpio_set_dir(HC06_EN_PIN, GPIO_OUT);
    gpio_put(HC06_EN_PIN, 0);

    /* Instala IRQ de RX */
    irq_set_exclusive_handler(UART1_IRQ, uart_rx_handler);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(HC06_UART_ID, true, false);

    /* Configura nome e PIN do HC-06 */
    hc06_config();

    /* Tarefa de inicialização se encerra */
    vTaskDelete(NULL);
}

/* ── MAIN ───────────────────────────────────────────────────── */
int main(void) {
    stdio_init_all();

    /* Inicializa todos os recursos RTOS no main — Rule 4.4 */
    xQueueButtons = xQueueCreate(20, sizeof(button_event_t));
    xQueueTX      = xQueueCreate(128, sizeof(uint8_t));
    xQueuePower   = xQueueCreate(1, sizeof(power_event_t));
    xRateSem      = xSemaphoreCreateCounting(MAX_BTN_EVENTS_PER_S,
                                              MAX_BTN_EVENTS_PER_S);

    /* init_task com prioridade máxima para configurar HC-06 antes */
    xTaskCreate(init_task,       "InitTask",     512, NULL, 4, NULL);
    xTaskCreate(tx_task,         "TXTask",       256, NULL, 3, NULL);
    xTaskCreate(imu_task,        "IMUTask",      512, NULL, 1, NULL);
    xTaskCreate(btn_task,        "BtnTask",      256, NULL, 1, NULL);
    xTaskCreate(power_task,      "PwrTask",      256, NULL, 2, NULL);
    xTaskCreate(rate_reset_task, "RateRstTask",  128, NULL, 2, NULL);

    vTaskStartScheduler();
    while (1);
}