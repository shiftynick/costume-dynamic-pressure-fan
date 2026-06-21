#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ble_nus.h"

// -------- pins --------
#define STATUS_LED      GPIO_NUM_10    // SparkFun Pro Micro ESP32-C3 STAT LED
#define I2C_SDA         GPIO_NUM_5     // Qwiic SDA
#define I2C_SCL         GPIO_NUM_6     // Qwiic SCL
#define FAN_PWM_GPIO    GPIO_NUM_7     // Adafruit MOSFET driver signal

// -------- LPS22 --------
#define LPS22_ADDR          0x5D
#define LPS22_WHO_AM_I      0x0F       // returns 0xB1 (HB) or 0xB3 (HH)
#define LPS22_CTRL_REG1     0x10
#define LPS22_PRESS_OUT_XL  0x28

// -------- fan PWM --------
#define FAN_PWM_FREQ_HZ      1000      // 1 kHz: gentle on the 1N4007 on the Adafruit breakout
#define FAN_PWM_TIMER        LEDC_TIMER_0
#define FAN_PWM_CHANNEL      LEDC_CHANNEL_0
#define FAN_PWM_MODE         LEDC_LOW_SPEED_MODE
#define FAN_PWM_RES_BITS     LEDC_TIMER_10_BIT
#define FAN_PWM_MAX_RAW      ((1 << 10) - 1)

// -------- control --------
#define TARGET_OVERPRESSURE_HPA   2.0f      // inflate to baseline + this
#define BOOST_BAND_HPA            0.5f      // if more than this below target, run flat-out
#define DUTY_MIN                  0.30f     // fan stall floor under load
#define DUTY_MAX                  1.00f
#define DUTY_FEEDFORWARD          0.55f     // initial guess at "hold" duty
#define KP                        0.50f     // duty per hPa of error
#define KI                        0.20f     // duty per (hPa*s) of error
#define INTEGRAL_CLAMP            5.0f
#define CONTROL_DT_MS             100

static const char *TAG = "graboid";

// ============================================================
// LPS22
// ============================================================
static esp_err_t lps22_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, 100);
}

static esp_err_t lps22_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t n)
{
    uint8_t addr = (n > 1) ? (reg | 0x80) : reg;
    return i2c_master_transmit_receive(dev, &addr, 1, out, n, 100);
}

static esp_err_t lps22_read_pressure_hpa(i2c_master_dev_handle_t dev, float *p_hpa_out, float *t_c_out)
{
    uint8_t b[5];
    esp_err_t err = lps22_read(dev, LPS22_PRESS_OUT_XL, b, 5);
    if (err != ESP_OK) return err;
    uint32_t raw_p = ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
    int16_t  raw_t = ((int16_t)b[4] << 8)  | b[3];
    *p_hpa_out = raw_p / 4096.0f;
    *t_c_out   = raw_t / 100.0f;
    return ESP_OK;
}

// ============================================================
// Fan PWM (low-side N-MOSFET via Adafruit PID 5648)
// ============================================================
static void fan_pwm_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = FAN_PWM_MODE,
        .timer_num       = FAN_PWM_TIMER,
        .duty_resolution = FAN_PWM_RES_BITS,
        .freq_hz         = FAN_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .speed_mode = FAN_PWM_MODE,
        .channel    = FAN_PWM_CHANNEL,
        .timer_sel  = FAN_PWM_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = FAN_PWM_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void fan_set_duty(float duty01)
{
    if (duty01 < 0.0f) duty01 = 0.0f;
    if (duty01 > 1.0f) duty01 = 1.0f;
    uint32_t raw = (uint32_t)(duty01 * FAN_PWM_MAX_RAW);
    ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL, raw);
    ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL);
}

// ============================================================
// app_main
// ============================================================
void app_main(void)
{
    // NVS is required by NimBLE for storing bonding info / settings.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    gpio_reset_pin(STATUS_LED);
    gpio_set_direction(STATUS_LED, GPIO_MODE_OUTPUT);

    fan_pwm_init();
    fan_set_duty(0.0f);

    // Start BLE advertising. Telemetry goes out as CSV via the NUS TX
    // characteristic. Inbound writes (commands) are wired up in chunk 2.
    ESP_ERROR_CHECK(ble_nus_init("graboid-01", NULL));

    // ---- I2C / LPS22 init ----
    i2c_master_bus_config_t bus_cfg = {
        .clk_source              = I2C_CLK_SRC_DEFAULT,
        .i2c_port                = I2C_NUM_0,
        .scl_io_num              = I2C_SCL,
        .sda_io_num              = I2C_SDA,
        .glitch_ignore_cnt       = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LPS22_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t lps22;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &lps22));

    // ---- LPS22 bring-up (non-fatal) ----
    // Don't crash-loop if the sensor isn't plugged in yet. Keep BLE alive and
    // the fan off, probing once a second until the LPS22 appears on the bus.
    bool sensor_ready = false;
    while (!sensor_ready) {
        uint8_t whoami = 0;
        if (lps22_read(lps22, LPS22_WHO_AM_I, &whoami, 1) != ESP_OK) {
            ESP_LOGW(TAG, "LPS22 not responding at 0x%02X - check Qwiic cable. Retrying...",
                     LPS22_ADDR);
            fan_set_duty(0.0f);
            gpio_set_level(STATUS_LED, 1);   // LED solid = waiting for sensor
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "LPS22 WHO_AM_I=0x%02X", whoami);

        // ODR=10Hz (010<<4), BDU=1
        if (lps22_write_reg(lps22, LPS22_CTRL_REG1, 0x22) != ESP_OK) {
            ESP_LOGW(TAG, "LPS22 config write failed; retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        sensor_ready = true;
    }
    ESP_LOGI(TAG, "LPS22 online; sampling baseline...");

    // ---- baseline ----
    // First few samples are ambient pressure with the fan off. Use these as
    // the reference so target tracks weather/altitude.
    vTaskDelay(pdMS_TO_TICKS(300));  // let ODR settle
    float baseline_hpa = 0.0f;
    {
        float p, t;
        const int n = 5;
        for (int i = 0; i < n; i++) {
            while (lps22_read_pressure_hpa(lps22, &p, &t) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            baseline_hpa += p;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        baseline_hpa /= n;
    }
    const float target_hpa = baseline_hpa + TARGET_OVERPRESSURE_HPA;
    ESP_LOGI(TAG, "baseline=%.2f hPa  target=%.2f hPa", baseline_hpa, target_hpa);

    // ---- control loop ----
    float integral = 0.0f;
    float duty     = DUTY_MIN;
    const float dt = CONTROL_DT_MS / 1000.0f;
    bool led       = false;

    while (1) {
        float p_hpa, t_c;
        if (lps22_read_pressure_hpa(lps22, &p_hpa, &t_c) == ESP_OK) {
            float error = target_hpa - p_hpa;

            if (error > BOOST_BAND_HPA) {
                // Far below target: run flat out, don't accumulate integral.
                duty     = DUTY_MAX;
                integral = 0.0f;
            } else {
                integral += error * dt;
                if (integral >  INTEGRAL_CLAMP) integral =  INTEGRAL_CLAMP;
                if (integral < -INTEGRAL_CLAMP) integral = -INTEGRAL_CLAMP;
                duty = DUTY_FEEDFORWARD + KP * error + KI * integral;
            }
            if (duty < DUTY_MIN) duty = DUTY_MIN;
            if (duty > DUTY_MAX) duty = DUTY_MAX;
            fan_set_duty(duty);

            ESP_LOGI(TAG, "P=%7.2f hPa  err=%+5.2f  duty=%3.0f%%  T=%4.1fC",
                     p_hpa, error, duty * 100.0f, t_c);

            // CSV for Bluefruit Connect's Plotter: target, measured, duty%, temp.
            char csv[64];
            int n = snprintf(csv, sizeof(csv), "%.2f,%.2f,%.1f,%.1f\n",
                             target_hpa, p_hpa, duty * 100.0f, t_c);
            if (n > 0 && n < (int)sizeof(csv)) ble_nus_send(csv);

            led = !led;
            gpio_set_level(STATUS_LED, led);
        } else {
            ESP_LOGW(TAG, "LPS22 read failed; cutting fan to safe");
            fan_set_duty(0.0f);
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_DT_MS));
    }
}
