/**
 * Battery monitor — reads AXP2101 PMU via shared I2C bus.
 *
 * AXP2101 registers used:
 *   0xA4 — Battery percentage (0–100)
 *   0x01 — Charger status (bit 5: charging active)
 *   0x00 — Power status (bit 2: battery present)
 */
#include "battery_monitor.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define AXP2101_ADDR            0x34

/* Registers */
#define REG_STATUS1             0x00
#define REG_STATUS2             0x01
#define REG_BAT_PERCENT         0xA4
#define REG_FUEL_GAUGE_CTRL     0xA2
#define REG_ADC_CHANNEL_CTRL    0x30

static i2c_master_dev_handle_t s_pmu_dev = NULL;

static esp_err_t pmu_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pmu_dev, &reg, 1, val, 1, pdMS_TO_TICKS(20));
}

static esp_err_t pmu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pmu_dev, buf, 2, pdMS_TO_TICKS(20));
}

esp_err_t battery_monitor_init(void)
{
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not available");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_pmu_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP2101 device: %s", esp_err_to_name(err));
        return err;
    }

    /* Verify AXP2101 is present by reading status register */
    uint8_t status = 0;
    err = pmu_read_reg(REG_STATUS1, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding at 0x%02X", AXP2101_ADDR);
        return err;
    }

    /* Enable fuel gauge */
    uint8_t fg_ctrl = 0;
    pmu_read_reg(REG_FUEL_GAUGE_CTRL, &fg_ctrl);
    fg_ctrl |= 0x01;  /* Enable fuel gauge */
    pmu_write_reg(REG_FUEL_GAUGE_CTRL, fg_ctrl);

    /* Enable battery voltage ADC measurement */
    uint8_t adc_ctrl = 0;
    pmu_read_reg(REG_ADC_CHANNEL_CTRL, &adc_ctrl);
    adc_ctrl |= 0x01;  /* Enable battery voltage channel */
    pmu_write_reg(REG_ADC_CHANNEL_CTRL, adc_ctrl);

    int pct = battery_get_percent();
    ESP_LOGI(TAG, "AXP2101 initialized (status=0x%02X, battery=%d%%)", status, pct);
    return ESP_OK;
}

int battery_get_percent(void)
{
    if (!s_pmu_dev) return -1;

    uint8_t pct = 0;
    esp_err_t err = pmu_read_reg(REG_BAT_PERCENT, &pct);
    if (err != ESP_OK) return -1;

    /* AXP2101 returns 0–100, but can sometimes report >100 */
    if (pct > 100) pct = 100;
    return (int)pct;
}

bool battery_is_charging(void)
{
    if (!s_pmu_dev) return false;

    uint8_t status = 0;
    if (pmu_read_reg(REG_STATUS2, &status) != ESP_OK) return false;

    /* Bits 6:5 of STATUS2: 00=standby, 01=charging, 10=charge done, 11=not charging */
    uint8_t chg_state = (status >> 5) & 0x03;
    return (chg_state == 0x01);
}
