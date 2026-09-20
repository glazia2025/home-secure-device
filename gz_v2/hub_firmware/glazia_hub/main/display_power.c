/**
 * display_power.c — see display_power.h.
 *
 * The LDO channel + I2C bus described here are the SAME physical resources the
 * camera used to own in camera_csi.c; the CAM_LDO_* / CAM_SCCB_* constants in
 * state.h remain the single source of truth for the pin/channel/voltage numbers.
 */
#include "display_power.h"
#include "state.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "DISP_PWR";

static esp_ldo_channel_handle_t s_ldo;
static i2c_master_bus_handle_t  s_i2c_bus;
static SemaphoreHandle_t        s_lock;

static void ensure_lock(void)
{
    /* First caller runs before any concurrency (display_init in main.c), so a
     * lazily-created mutex is safe here. */
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

esp_err_t display_power_acquire_mipi_phy(void)
{
    ensure_lock();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t ret = ESP_OK;
    if (!s_ldo) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = CAM_LDO_CHAN_ID,
            .voltage_mv = CAM_LDO_VOLTAGE_MV,
        };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MIPI D-PHY LDO acquire failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "MIPI D-PHY LDO ch%d @ %dmV acquired (shared DSI+CSI)",
                     CAM_LDO_CHAN_ID, CAM_LDO_VOLTAGE_MV);
        }
    }

    if (s_lock) xSemaphoreGive(s_lock);
    return ret;
}

esp_ldo_channel_handle_t display_power_ldo_handle(void)
{
    return s_ldo;
}

i2c_master_bus_handle_t display_i2c_bus(void)
{
    ensure_lock();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_i2c_bus) {
        i2c_master_bus_config_t i2c_conf = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .sda_io_num = CAM_SCCB_SDA_GPIO,
            .scl_io_num = CAM_SCCB_SCL_GPIO,
            .i2c_port   = CAM_SCCB_I2C_PORT,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t ret = i2c_new_master_bus(&i2c_conf, &s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "shared I2C%d bus create failed: %s",
                     CAM_SCCB_I2C_PORT, esp_err_to_name(ret));
            s_i2c_bus = NULL;
        } else {
            ESP_LOGI(TAG, "shared I2C%d bus up: SDA=%d SCL=%d (touch/backlight/SCCB)",
                     CAM_SCCB_I2C_PORT, CAM_SCCB_SDA_GPIO, CAM_SCCB_SCL_GPIO);
        }
    }

    i2c_master_bus_handle_t bus = s_i2c_bus;
    if (s_lock) xSemaphoreGive(s_lock);
    return bus;
}
