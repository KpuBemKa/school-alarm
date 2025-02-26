#include <cstring>
#include <ctime>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
// #include "file_serving_example_common.h"
#include "driver/gpio.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "config.hpp"
#include "http_controller.hpp"
#include "schedule.hpp"
#include "settings.hpp"
#include "sleep.hpp"
#include "sntp.hpp"
#include "wifi_controller.hpp"

static const char TAG[] = "MAIN";

#define LOG_I(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LOG_W(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LOG_E(...) ESP_LOGE(TAG, __VA_ARGS__)

bool s_button_pressed_flag = false;

settings::Settings s_settings;
Schedule s_schedule;
wifi::WifiController s_wifi_controller(s_settings);
srvr::HttpController s_http_controller(s_schedule, s_settings);
SleepController s_sleep_controller(s_wifi_controller, s_http_controller, s_schedule);
wifi::SNTP s_sntp(s_settings);

TaskHandle_t s_schedule_task_handle;
TaskHandle_t s_server_task_handle;
TaskHandle_t s_sleep_task_handle;
TaskHandle_t s_sntp_task_handle;

esp_err_t
MountSpiffsStorage(const char* base_path);

void
ScheduleTask(void* args);
void
HttpServerTask(void* args);
void
SntpTask(void* args);

void
RingAlarm(const AlarmType alarm_type);

extern long timezone;

extern "C" void
app_main(void)
{
    // Configure GPIO pins
    esp_err_t esp_result = gpio_set_direction(pins::BUTTON, gpio_mode_t::GPIO_MODE_INPUT);
    esp_result |= gpio_set_pull_mode(pins::BUTTON, gpio_pull_mode_t::GPIO_PULLUP_ONLY);
    if (esp_result != ESP_OK) {
        LOG_E("Failed to configure the button pin: %s", esp_err_to_name(esp_result));
    }

    // vTaskDelay(pdMS_TO_TICKS(1'000));
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Initialize file storage */
    ESP_ERROR_CHECK(MountSpiffsStorage(SPIFFS_BASE_PATH.data()));

    ESP_ERROR_CHECK(s_wifi_controller.Init());

    s_schedule.Load();
    s_settings.Load();
    s_sleep_controller.SetSleepTimeout(pdMS_TO_TICKS(60'000));
    s_sntp.SetTimezone(s_settings.GetSettings().timezone);

    ESP_ERROR_CHECK(s_wifi_controller.Start());
    ESP_ERROR_CHECK(s_http_controller.StartServer());
    if (s_wifi_controller.IsConnected()) {
        s_sntp.Init(20'000);
    }

    xTaskCreate(ScheduleTask, "SCHEDULE", 8192, nullptr, 8, &s_schedule_task_handle);
    xTaskCreate(HttpServerTask, "SERVER", 16384, nullptr, 8, &s_schedule_task_handle);
    xTaskCreate(s_sleep_controller.TaskRunner, "SLEEP", 8192, &s_sleep_controller, 8, &s_sleep_task_handle);
    xTaskCreate(SntpTask, "SNTP", 8192, nullptr, 8, &s_sntp_task_handle);

    while (true) {
        if (gpio_get_level(pins::BUTTON) == 0) {
            s_button_pressed_flag = true;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void
ScheduleTask(void* args)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (s_schedule.IsEmpty())
            continue;

        const auto schedule_point = s_schedule.GetSchedulePoint();
        if (s_schedule.GetSystemMinute() != schedule_point.day_minute)
            continue;

        s_sleep_controller.ResetSleepTimeout();
        RingAlarm(schedule_point.alarm_type);

        if (s_schedule.GetPointsCount() == 1) {
            // LOG_I("Sleeping for a minute...");
            vTaskDelay(pdMS_TO_TICKS(60 * 1'000));
            // LOG_I("Woken");
        }

        s_schedule.AdvanceSchedule();
    }
}

void
HttpServerTask(void* args)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1'000));

        if (s_wifi_controller.GetConnectionsCount() > 0) {
            // LOG_I("Resetting...");
            s_sleep_controller.ResetSleepTimeout();
        }

        if (!s_button_pressed_flag)
            continue;

        LOG_I("Button was pressed. Starting the server...");
        s_button_pressed_flag = false;

        if (!s_wifi_controller.IsStarted()) {
            ESP_ERROR_CHECK(s_wifi_controller.Start());
            ESP_ERROR_CHECK(s_http_controller.StartServer());
        }
    }
}

void
SntpTask(void* args)
{
    constexpr TickType_t kSyncInterval = pdMS_TO_TICKS(24 * 60 * 60 * 1'000);
    static TickType_t next_time_sync = xTaskGetTickCount() + kSyncInterval;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10'000));

        if (xTaskGetTickCount() < next_time_sync)
            continue;

        // check if wi-fi is connected
        if (!s_wifi_controller.IsConnected()) {
            // try to start it
            const esp_err_t result = s_wifi_controller.Start();

            // if it has been started, but still not connected, it does not have anything to connect to
            if (result == ESP_OK && !s_wifi_controller.IsConnected()) {
                LOG_W("%s:%d | Wi-Fi is not supposed to be connected. Skipping SNTP sync...", __FILE__, __LINE__);
                next_time_sync = xTaskGetTickCount() + pdMS_TO_TICKS(60 * 60 * 1'000);
                continue;
            }

            // if it has failed, however, schedule the next sync in an hour
            if (result != ESP_OK) {
                LOG_W("%s:%d | Failed to start Wi-Fi to start the system time synchronization: %s",
                      __FILE__,
                      __LINE__,
                      esp_err_to_name(result));
                next_time_sync = xTaskGetTickCount() + pdMS_TO_TICKS(60 * 60 * 1'000);
                continue;
            }
        }

        const esp_err_t result = s_sntp.Resync();
        next_time_sync = xTaskGetTickCount() + kSyncInterval;

        if (result != ESP_OK) {
            LOG_E("System time syncronization failed.");
        }
    }
}

esp_err_t
MountSpiffsStorage(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = { .base_path = base_path,
                                   .partition_label = NULL,
                                   .max_files = 5, // This sets the maximum number of files
                                                   // that can be open at the same time
                                   .format_if_mount_failed = true };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

void
RingAlarm(const AlarmType alarm_type)
{
    LOG_I("Ringing the alarm...");
    std::size_t cycles = static_cast<uint16_t>(alarm_type);

    for (std::size_t i = 0; i < cycles; ++i) {
        gpio_set_level(pins::RELAY, 1);
        vTaskDelay(pdMS_TO_TICKS(1'000));
        gpio_set_level(pins::RELAY, 0);
        vTaskDelay(pdMS_TO_TICKS(1'000));
    }
}