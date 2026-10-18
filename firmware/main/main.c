#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "door.h"
#include "events.h"
#include "keys.h"
#include "net.h"
#include "notify.h"
#include "remote.h"
#include "settings.h"
#include "util.h"
#include "web.h"

#define KEEP_FIRMWARE_AFTER_US (60 * 1000000LL)   // see CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#define LOG_START_REASON_MS 5000

static const char *TAG = "main";

static void on_change(void)
{
    web_notify();
    remote_notify();
}

static void keep_firmware(void *arg)
{
    esp_ota_mark_app_valid_cancel_rollback();
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_SW:
        return "restart";
    case ESP_RST_PANIC:
        return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    util_device_id();   // read the MAC once, before other tasks ask for it
    settings_init();
    keys_init();
    door_set_listener(on_change);
    events_set_listener(on_change);

    door_start();   // call lines, the door, the button and the LED work from here, Wi-Fi or not
    net_start();
    web_start();
    remote_start();
    notify_start();

    event_t boot = { .type = EV_BOOT };
    strlcpy(boot.who, reset_reason(), sizeof(boot.who));
    events_post(&boot);

    // A firmware uploaded from the app that crashes before this would be rolled back.
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = keep_firmware, .name = "keep_fw" };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, KEEP_FIRMWARE_AFTER_US);
    }

    // After a reset the USB console comes back a moment later, and `idf.py monitor` resets the
    // board when it starts, so the reason is logged once the monitor has had time to reconnect.
    vTaskDelay(pdMS_TO_TICKS(LOG_START_REASON_MS));
    if (!strcmp(boot.who, "power-on") || !strcmp(boot.who, "restart")) {
        ESP_LOGI(TAG, "started after: %s", boot.who);
    } else {
        ESP_LOGW(TAG, "started after: %s (after a crash, `idf.py coredump-info` shows where)", boot.who);
    }
}
