#include "display_power.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "ui_screens.h"

static const char *TAG = "disp_pwr";

static esp_timer_handle_t s_sleep_timer = NULL;
static bool s_display_on = true;

static void sleep_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Inactivity timeout — sleeping display");
    display_sleep();
}

void display_power_init(void)
{
    esp_timer_create_args_t timer_args = {
        .callback = sleep_timer_cb,
        .name = "disp_sleep",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sleep_timer));

    /* Start the first countdown */
    display_activity_signal();
    ESP_LOGI(TAG, "Display power management init (timeout=%ds)",
             DISPLAY_TIMEOUT_MS / 1000);
}

void display_activity_signal(void)
{
    /* Wake if sleeping */
    if (!s_display_on) {
        ESP_LOGI(TAG, "Activity signal while sleeping — waking display");
        display_wake();
    }

    /* Reset countdown */
    esp_timer_stop(s_sleep_timer);
    esp_timer_start_once(s_sleep_timer, (uint64_t)DISPLAY_TIMEOUT_MS * 1000ULL);
}

void display_sleep(void)
{
    if (!s_display_on) return;
    s_display_on = false;

    ui_display_sleep();
    ESP_LOGI(TAG, "Display off");
}

void display_wake(void)
{
    if (s_display_on) return;
    s_display_on = true;

    ui_display_wake();
    ESP_LOGI(TAG, "Display on");
}

bool display_is_on(void)
{
    return s_display_on;
}
