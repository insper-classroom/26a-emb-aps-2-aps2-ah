/*
 * main.c (FIRMWARE DE TESTE - bare-metal, sem FreeRTOS)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"

#define BTN_APPROVE   13
#define BTN_DENY      15
#define BTN_CLICK     14
#define BTN_INSPECT   12
#define BTN_POWER     11

#define LED_PIN       17
#define LED_CALIBRADO 16

#define I2C_PORT      i2c0
#define I2C_SDA       8
#define I2C_SCL       9
#define MPU_ADDR      0x68

#define HC06_UART     uart1
#define HC06_TX       4
#define HC06_RX       5
#define HC06_STATE    3
#define HC06_BAUD     9600

#define CALIB_SAMPLES 200

const uint button_pins[] = {BTN_APPROVE, BTN_DENY, BTN_CLICK, BTN_INSPECT, BTN_POWER};
const char *button_names[] = {"APPROVE", "DENY", "CLICK", "INSPECT", "POWER"};
#define NUM_BUTTONS (sizeof(button_pins) / sizeof(button_pins[0]))

bool button_last[NUM_BUTTONS];

int32_t gyro_offset_x = 0;
int32_t gyro_offset_y = 0;
int32_t gyro_offset_z = 0;
bool gyro_calibrated = false;

void mpu6050_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    i2c_write_blocking(I2C_PORT, MPU_ADDR, buf, 2, false);
}

void mpu6050_read(uint8_t reg, uint8_t *buf, uint8_t len) {
    i2c_write_blocking(I2C_PORT, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, MPU_ADDR, buf, len, false);
}

void mpu6050_init(void) {
    mpu6050_write(0x6B, 0x00);
}

void calibrate_gyro(void) {
    printf("[CALIB] Iniciando calibracao... mantenha o controle PARADO.\n");
    int32_t sum_x = 0, sum_y = 0, sum_z = 0;
    for (int i = 0; i < CALIB_SAMPLES; i++) {
        uint8_t buf[6];
        mpu6050_read(0x43, buf, 6);
        sum_x += (int16_t)((buf[0] << 8) | buf[1]);
        sum_y += (int16_t)((buf[2] << 8) | buf[3]);
        sum_z += (int16_t)((buf[4] << 8) | buf[5]);
        if (i % 20 == 0) {
            gpio_put(LED_CALIBRADO, !gpio_get(LED_CALIBRADO));
        }
        sleep_ms(5);
    }
    gyro_offset_x = sum_x / CALIB_SAMPLES;
    gyro_offset_y = sum_y / CALIB_SAMPLES;
    gyro_offset_z = sum_z / CALIB_SAMPLES;
    gyro_calibrated = true;
    gpio_put(LED_CALIBRADO, 1);
    printf("[CALIB] Concluida! Offsets -> x=%d  y=%d  z=%d\n",
           (int)gyro_offset_x, (int)gyro_offset_y, (int)gyro_offset_z);
}

void setup_buttons(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
        button_last[i] = true;
    }
}

void setup_led(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    gpio_init(LED_CALIBRADO);
    gpio_set_dir(LED_CALIBRADO, GPIO_OUT);
    gpio_put(LED_CALIBRADO, 0);
}

void setup_i2c(void) {
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
}

void setup_hc06(void) {
    uart_init(HC06_UART, HC06_BAUD);
    gpio_set_function(HC06_TX, GPIO_FUNC_UART);
    gpio_set_function(HC06_RX, GPIO_FUNC_UART);
    gpio_init(HC06_STATE);
    gpio_set_dir(HC06_STATE, GPIO_IN);
}

void test_mpu6050(void) {
    uint8_t who = 0;
    mpu6050_read(0x75, &who, 1);
    printf("[MPU6050] WHO_AM_I = 0x%02X ", who);
    if (who == 0x68) {
        printf("-> OK! Sensor respondendo.\n");
    } else {
        printf("-> ERRO! Esperado 0x68.\n");
    }
}

void read_gyro(void) {
    uint8_t ax_h, ax_l, ay_h, ay_l, az_h, az_l;
    mpu6050_read(0x3B, &ax_h, 1);
    mpu6050_read(0x3C, &ax_l, 1);
    mpu6050_read(0x3D, &ay_h, 1);
    mpu6050_read(0x3E, &ay_l, 1);
    mpu6050_read(0x3F, &az_h, 1);
    mpu6050_read(0x40, &az_l, 1);
    int16_t ax = (int16_t)((ax_h << 8) | ax_l);
    int16_t ay = (int16_t)((ay_h << 8) | ay_l);
    int16_t az = (int16_t)((az_h << 8) | az_l);
    printf("[ACC] x=%6d  y=%6d  z=%6d\n", ax, ay, az);
}

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    printf("\n=== TESTE DE HARDWARE - Controle Papers Please ===\n");

    setup_buttons();
    setup_led();
    setup_i2c();
    setup_hc06();

    mpu6050_init();
    test_mpu6050();
    calibrate_gyro();

    uart_puts(HC06_UART, "HC06 OK\n");
    printf("[HC-06] Enviei 'HC06 OK' pela UART1.\n");
    printf("[HC-06] STATE atual = %d\n", gpio_get(HC06_STATE));

    uint32_t last_gyro = 0;

    while (true) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            bool pressed = !gpio_get(button_pins[i]);
            if (pressed != !button_last[i]) {
                if (pressed) {
                    printf("[BOTAO] %s pressionado\n", button_names[i]);
                } else {
                    printf("[BOTAO] %s solto\n", button_names[i]);
                }
                button_last[i] = !pressed;
            }
        }

        gpio_put(LED_PIN, !gpio_get(BTN_APPROVE));

        if (uart_is_readable(HC06_UART)) {
            char c = uart_getc(HC06_UART);
            printf("[HC-06] recebeu: '%c'\n", c);
        }

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_gyro >= 1000) {
            read_gyro();
            uart_puts(HC06_UART, "TESTE,123\n");   // <<< LINHA DE TESTE: manda pelo Bluetooth 1x/s
            printf("[HC-06] STATE = %d\n", gpio_get(HC06_STATE));
            last_gyro = now;
        }

        sleep_ms(10);
    }
    return 0;
}