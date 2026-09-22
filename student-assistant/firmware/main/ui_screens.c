#include "ui_screens.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"

static const char *TAG = "ui";

/*
 * Display pins — from Waveshare factory pin_config.h
 * CO5300 AMOLED, 410x502, QSPI @80MHz, RGB565
 */
#define LCD_CS     GPIO_NUM_12
#define LCD_SCK    GPIO_NUM_11
#define LCD_D0     GPIO_NUM_4
#define LCD_D1     GPIO_NUM_5
#define LCD_D2     GPIO_NUM_6
#define LCD_D3     GPIO_NUM_7
#define LCD_RST    GPIO_NUM_8
#define LCD_WIDTH  410
#define LCD_HEIGHT 502

/*
 * Touch pins — FT3168 on I2C
 */
#define TOUCH_SDA  GPIO_NUM_15
#define TOUCH_SCL  GPIO_NUM_14
#define TOUCH_INT  GPIO_NUM_38
#define TOUCH_RST  GPIO_NUM_9
#define TOUCH_ADDR 0x38

/* Colors (matching web UI) */
#define COLOR_PURPLE  lv_color_hex(0x7F77DD)
#define COLOR_TEAL    lv_color_hex(0x1D9E75)
#define COLOR_AMBER   lv_color_hex(0xEF9F27)
#define COLOR_CORAL   lv_color_hex(0xD85A30)
#define COLOR_BG      lv_color_hex(0x0A0A0A)
#define COLOR_SURFACE lv_color_hex(0x1E1E1E)
#define COLOR_DIM     lv_color_hex(0x888888)

/* Top safe margin — display has rounded corners, push content down */
#define TOP_SAFE  28
#define LEFT_SAFE 36
#define RIGHT_SAFE 36

/* Screen objects */
static lv_obj_t *scr_home      = NULL;
static lv_obj_t *scr_recording = NULL;
static lv_obj_t *scr_processing = NULL;
static lv_obj_t *scr_saved     = NULL;
static lv_obj_t *scr_today     = NULL;
static lv_obj_t *scr_offline   = NULL;

/* Home screen widgets */
static lv_obj_t *home_clock_label  = NULL;
static lv_obj_t *home_date_label   = NULL;
static lv_obj_t *home_wifi_icon    = NULL;
static lv_obj_t *home_batt_icon    = NULL;
static lv_obj_t *home_items_cont   = NULL;
static lv_obj_t *home_more_label __attribute__((unused)) = NULL;

/* Recording screen widgets */
static lv_obj_t *rec_timer_label   = NULL;
static lv_obj_t *rec_hint_label    = NULL;
static lv_obj_t *rec_bars[5];
static lv_obj_t *rec_wifi_icon     = NULL;
static lv_obj_t *rec_batt_icon     = NULL;

/* Processing screen widgets */
static lv_obj_t *proc_stt_bar     = NULL;
static lv_obj_t *proc_intent_bar  = NULL;
static lv_obj_t *proc_stt_row     = NULL;
static lv_obj_t *proc_int_row     = NULL;
static lv_obj_t *proc_title_label = NULL;
static lv_obj_t *proc_dots_label  = NULL;
static lv_obj_t *proc_answer_label = NULL;
static lv_obj_t *proc_speaker_bg   = NULL;  /* Teal circle behind speaker icon */
static lv_obj_t *proc_speaker_icon = NULL;  /* Speaker symbol */

/* Saved screen widgets */
static lv_obj_t *saved_badge_label = NULL;
static lv_obj_t *saved_title_label = NULL;
static lv_obj_t *saved_date_label  = NULL;
static lv_obj_t *saved_sync_label  = NULL;

/* Offline screen widgets */
static lv_obj_t *offline_count_label = NULL;

/* Finished screen widgets */
static lv_obj_t *scr_finished = NULL;
static lv_obj_t *finished_items_cont = NULL;

/* LVGL display/touch drivers */
static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev = NULL;

/* LVGL mutex — all LVGL calls from outside lvgl_task must hold this */
static SemaphoreHandle_t s_lvgl_mutex = NULL;

/* Item action callback */
static ui_item_action_cb_t s_item_action_cb = NULL;

/* Action bar callbacks (registered by main.c) */
static ui_action_cb_t s_record_cb   = NULL;
static ui_action_cb_t s_listen_cb   = NULL;
static ui_action_cb_t s_read_all_cb = NULL;

/* Keep a copy of items for action callbacks and today screen */
#define MAX_ITEMS 15
static ui_item_t s_items_copy[MAX_ITEMS];
static char s_item_ids[MAX_ITEMS][48];
static char s_item_types[MAX_ITEMS][16];
static char s_item_categories[MAX_ITEMS][16];
static char s_item_titles[MAX_ITEMS][128];
static char s_item_dates[MAX_ITEMS][24];
static char s_item_recurrences[MAX_ITEMS][32];
static int  s_item_paused[MAX_ITEMS];
static int  s_item_done[MAX_ITEMS];
static int  s_items_count = 0;

/* Track previous clock text to avoid unnecessary redraws */
static char s_prev_clock[16] = "";
static char s_prev_date[32] = "";

/* ---------- Domain state ---------- */

typedef struct {
    const char *id;
    const char *label;  /* short label for badge */
} domain_info_t;

static const domain_info_t DOMAIN_INFO[UI_DOMAIN_COUNT] = {
    [UI_DOMAIN_SOCCER]    = { "sports",   "Sports"  },
    [UI_DOMAIN_MUSIC]     = { "music",     "Music"   },
    [UI_DOMAIN_CLASSROOM] = { "classroom", "Class"   },
    [UI_DOMAIN_GENERAL]   = { "general",  "General" },
};

static ui_domain_t s_current_domain = UI_DOMAIN_GENERAL;

/* Domain pill buttons (4 on home screen) */
static lv_obj_t *home_domain_btns[UI_DOMAIN_COUNT];
static lv_obj_t *home_domain_btn_labels[UI_DOMAIN_COUNT];

/* Action bar buttons */
static lv_obj_t *home_record_btn       = NULL;
static lv_obj_t *home_record_btn_label = NULL;
static lv_obj_t *home_listen_btn       = NULL;
static lv_obj_t *home_read_btn         = NULL;

/* Session summary screen */
static lv_obj_t *scr_session          = NULL;
static lv_obj_t *session_header_label = NULL;
static lv_obj_t *session_items_cont   = NULL;

void ui_lock(void)   { if (s_lvgl_mutex) xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY); }
void ui_unlock(void) { if (s_lvgl_mutex) xSemaphoreGive(s_lvgl_mutex); }

/* ---------- LVGL tick & task ---------- */

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void lvgl_task(void *arg)
{
    while (1) {
        ui_lock();
        uint32_t next_ms = lv_timer_handler();
        ui_unlock();
        /* Sleep for the time LVGL tells us, clamped to [2..20] ms */
        if (next_ms < 2) next_ms = 2;
        if (next_ms > 20) next_ms = 20;
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

/* ---------- Clock timer ---------- */

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    if (home_clock_label) {
        char buf[16];
        int h = t->tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, sizeof(buf), "%d:%02d %s",
                 h, t->tm_min, t->tm_hour >= 12 ? "PM" : "AM");
        /* Only update if changed — prevents flicker */
        if (strcmp(buf, s_prev_clock) != 0) {
            lv_label_set_text(home_clock_label, buf);
            strncpy(s_prev_clock, buf, sizeof(s_prev_clock));
        }
    }
    if (home_date_label) {
        static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
        static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        char buf[32];
        snprintf(buf, sizeof(buf), "%s, %s %d",
                 days[t->tm_wday], months[t->tm_mon], t->tm_mday);
        if (strcmp(buf, s_prev_date) != 0) {
            lv_label_set_text(home_date_label, buf);
            strncpy(s_prev_date, buf, sizeof(s_prev_date));
        }
    }
}

/* ---------- Display flush callback ---------- */

static esp_lcd_panel_io_handle_t s_panel_io __attribute__((unused)) = NULL;
static esp_lcd_panel_handle_t s_panel __attribute__((unused)) = NULL;
static spi_device_handle_t s_spi_dev = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;

/* Forward declarations */
static void co5300_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

#define FLUSH_CHUNK_ROWS  20

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    size_t row_bytes = w * 2;

    if (!s_spi_dev) {
        lv_display_flush_ready(disp);
        return;
    }

    lv_draw_sw_rgb565_swap(px_map, w * h);
    co5300_set_window(x1, y1, x2, y2);
    gpio_set_level(LCD_CS, 0);

    bool first = true;
    for (int row = 0; row < h; row += FLUSH_CHUNK_ROWS) {
        int chunk_h = (row + FLUSH_CHUNK_ROWS <= h) ? FLUSH_CHUNK_ROWS : (h - row);
        size_t chunk_bytes = row_bytes * chunk_h;

        if (first) {
            spi_transaction_t t = {
                .flags = SPI_TRANS_MODE_QIO,
                .cmd = 0x32,
                .addr = 0x002C00,
                .length = chunk_bytes * 8,
                .tx_buffer = px_map,
            };
            spi_device_polling_transmit(s_spi_dev, &t);
            first = false;
        } else {
            spi_transaction_ext_t t = {
                .base = {
                    .flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD
                             | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY,
                    .length = chunk_bytes * 8,
                    .tx_buffer = px_map + (row * row_bytes),
                },
                .command_bits = 0,
                .address_bits = 0,
                .dummy_bits = 0,
            };
            spi_device_polling_transmit(s_spi_dev, (spi_transaction_t *)&t);
        }
    }

    gpio_set_level(LCD_CS, 1);
    lv_display_flush_ready(disp);
}

/* ---------- Touch read callback ---------- */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t buf[6];
    uint8_t reg = 0x02;

    if (!s_touch_dev) { data->state = LV_INDEV_STATE_RELEASED; return; }
    esp_err_t err = i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, 6, pdMS_TO_TICKS(10));
    if (err != ESP_OK) { data->state = LV_INDEV_STATE_RELEASED; return; }

    uint8_t touch_count = buf[0] & 0x0F;
    if (touch_count > 0) {
        uint16_t x = ((buf[1] & 0x0F) << 8) | buf[2];
        uint16_t y = ((buf[3] & 0x0F) << 8) | buf[4];
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;

        /* Signal user activity to reset display sleep timer.
         * Only reset timer if display is already on — touch should NOT wake
         * a sleeping display (button only). This prevents phantom touch wakes. */
        extern void display_activity_signal(void);
        extern bool display_is_on(void);
        if (display_is_on()) {
            display_activity_signal();
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ---------- Display & touch hardware init ---------- */

static spi_device_handle_t s_cmd_dev = NULL;

static void co5300_send_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR,
        .cmd = 0x02,
        .addr = (uint32_t)cmd << 8,
        .length = len * 8,
        .tx_buffer = data,
    };

    spi_device_handle_t dev = s_cmd_dev ? s_cmd_dev : s_spi_dev;
    if (dev) {
        gpio_set_level(LCD_CS, 0);
        spi_device_polling_transmit(dev, &t);
        gpio_set_level(LCD_CS, 1);
    }
}

static void co5300_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    x1 += 22; x2 += 22;
    uint8_t col_data[] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t row_data[] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    co5300_send_cmd(0x2A, col_data, 4);
    co5300_send_cmd(0x2B, row_data, 4);
}

static void init_display_hw(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_SCK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_WIDTH * FLUSH_CHUNK_ROWS * 2 + 64,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << LCD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(LCD_CS, 1);

    spi_device_interface_config_t cmd_cfg = {
        .command_bits = 8,
        .address_bits = 24,
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &cmd_cfg, &s_cmd_dev));

    co5300_send_cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t pixfmt[] = { 0x55 };
    co5300_send_cmd(0x3A, pixfmt, 1);

    uint8_t spi_mode[] = { 0x80 };
    co5300_send_cmd(0xC4, spi_mode, 1);

    uint8_t ctrl_disp[] = { 0x20 };
    co5300_send_cmd(0x53, ctrl_disp, 1);

    uint8_t hbm_bright[] = { 0xFF };
    co5300_send_cmd(0x63, hbm_bright, 1);

    co5300_send_cmd(0x29, NULL, 0);

    uint8_t brightness[] = { 0xD0 };
    co5300_send_cmd(0x51, brightness, 1);

    uint8_t contrast[] = { 0x00 };
    co5300_send_cmd(0x58, contrast, 1);

    vTaskDelay(pdMS_TO_TICKS(10));

    spi_bus_remove_device(s_cmd_dev);
    s_cmd_dev = NULL;

    spi_device_interface_config_t dev_cfg = {
        .command_bits = 8,
        .address_bits = 24,
        .clock_speed_hz = 80 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev));

    spi_device_acquire_bus(s_spi_dev, portMAX_DELAY);

    ESP_LOGI(TAG, "Display HW init: CO5300 QSPI %dx%d (col_offset=22)",
             LCD_WIDTH, LCD_HEIGHT);
}

static void init_touch_hw(void)
{
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_touch_dev));

    gpio_config_t trst_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&trst_cfg);
    gpio_set_level(TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t tint_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&tint_cfg);

    ESP_LOGI(TAG, "Touch init: FT3168 I2C addr=0x%02X (new driver, BSP I2C bus)",
             TOUCH_ADDR);
}

/* ---------- Helpers ---------- */

static lv_color_t type_color(const char *type)
{
    if (!type) return COLOR_DIM;
    if (strcmp(type, "reminder") == 0) return COLOR_PURPLE;
    if (strcmp(type, "event") == 0) return COLOR_TEAL;
    if (strcmp(type, "note") == 0) return COLOR_AMBER;
    return COLOR_DIM;
}

/* ---------- Item action event handlers ---------- */

typedef struct {
    int item_index;
    char action[8];
} item_action_data_t;

static void item_action_event_cb(lv_event_t *e)
{
    item_action_data_t *d = (item_action_data_t *)lv_event_get_user_data(e);
    if (!d || !s_item_action_cb) return;
    if (d->item_index < 0 || d->item_index >= s_items_count) return;
    s_item_action_cb(s_item_ids[d->item_index], d->action);
}

#define MAX_ACTION_DATA 30
static item_action_data_t s_action_pool[MAX_ACTION_DATA];
static int s_action_pool_idx = 0;

static item_action_data_t *alloc_action_data(int index, const char *action)
{
    if (s_action_pool_idx >= MAX_ACTION_DATA) s_action_pool_idx = 0;
    item_action_data_t *d = &s_action_pool[s_action_pool_idx++];
    d->item_index = index;
    strncpy(d->action, action, sizeof(d->action) - 1);
    d->action[sizeof(d->action) - 1] = '\0';
    return d;
}

/* ---------- "More" button click → show Today screen ---------- */

static void __attribute__((unused)) more_click_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load_anim(scr_today, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

/* ---------- Today screen back button ---------- */

static void today_back_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load_anim(scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

/* ---------- Finished screen callbacks ---------- */

static void __attribute__((unused)) finished_click_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load_anim(scr_finished, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void finished_back_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load_anim(scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

/* ---------- Tap to toggle action buttons ---------- */

typedef struct {
    lv_obj_t *btn_row;
    bool revealed;
} tap_toggle_data_t;

#define MAX_TAP_DATA 20
static tap_toggle_data_t s_tap_pool[MAX_TAP_DATA];
static int s_tap_pool_idx = 0;

/* Keep track of currently open row so only one is open at a time */
static tap_toggle_data_t *s_open_row = NULL;

static void item_tap_cb(lv_event_t *e)
{
    tap_toggle_data_t *td = (tap_toggle_data_t *)lv_event_get_user_data(e);
    if (!td || !td->btn_row) return;

    if (td->revealed) {
        /* Close this row */
        lv_obj_add_flag(td->btn_row, LV_OBJ_FLAG_HIDDEN);
        td->revealed = false;
        s_open_row = NULL;
    } else {
        /* Close previously open row first */
        if (s_open_row && s_open_row != td && s_open_row->btn_row) {
            lv_obj_add_flag(s_open_row->btn_row, LV_OBJ_FLAG_HIDDEN);
            s_open_row->revealed = false;
        }
        /* Open this row */
        lv_obj_remove_flag(td->btn_row, LV_OBJ_FLAG_HIDDEN);
        td->revealed = true;
        s_open_row = td;
    }
}

/* Forward declarations for callbacks used before their definitions */
static void __attribute__((unused)) domain_badge_click_cb(lv_event_t *e);
static void domain_pill_cb(lv_event_t *e);
static void record_btn_cb(lv_event_t *e);
static void __attribute__((unused)) listen_btn_cb(lv_event_t *e);
static void read_btn_cb(lv_event_t *e);
static void session_home_cb(lv_event_t *e);
static void session_all_cb(lv_event_t *e);
static void update_domain_buttons(void);

/* Helper: create one action bar button */
static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *label_text,
                                  lv_color_t color)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, 60);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, color, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_TOP, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ---------- Screen builders ---------- */

static void create_screen_home(void)
{
    scr_home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_home, COLOR_BG, 0);

    /* === STATUS BAR: date (left), wifi + battery (right) y=28 h=30 === */
    home_date_label = lv_label_create(scr_home);
    lv_label_set_text(home_date_label, "");
    lv_obj_set_style_text_color(home_date_label, COLOR_DIM, 0);
    lv_obj_set_style_text_font(home_date_label, &lv_font_montserrat_14, 0);
    lv_obj_align(home_date_label, LV_ALIGN_TOP_LEFT, LEFT_SAFE, TOP_SAFE + 2);

    home_batt_icon = lv_label_create(scr_home);
    lv_label_set_text(home_batt_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(home_batt_icon, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(home_batt_icon, &lv_font_montserrat_16, 0);
    lv_obj_align(home_batt_icon, LV_ALIGN_TOP_RIGHT, -RIGHT_SAFE, TOP_SAFE + 2);

    home_wifi_icon = lv_label_create(scr_home);
    lv_label_set_text(home_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(home_wifi_icon, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(home_wifi_icon, &lv_font_montserrat_16, 0);
    lv_obj_align_to(home_wifi_icon, home_batt_icon, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    /* === CLOCK: large centered y=58 h=54 === */
    home_clock_label = lv_label_create(scr_home);
    lv_obj_set_style_text_font(home_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(home_clock_label, lv_color_white(), 0);
    lv_obj_align(home_clock_label, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 26);
    lv_label_set_text(home_clock_label, "12:00 PM");

    /* === DOMAIN BAR: 4 pill buttons y=115 h=46 === */
    lv_obj_t *domain_cont = lv_obj_create(scr_home);
    lv_obj_remove_style_all(domain_cont);
    lv_obj_set_size(domain_cont, LCD_WIDTH - LEFT_SAFE - RIGHT_SAFE, 40);
    lv_obj_set_flex_flow(domain_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(domain_cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(domain_cont, 4, 0);
    lv_obj_align(domain_cont, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 88);

    for (int i = 0; i < UI_DOMAIN_COUNT; i++) {
        lv_obj_t *btn = lv_obj_create(domain_cont);
        lv_obj_remove_style_all(btn);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 36);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, DOMAIN_INFO[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        home_domain_btns[i] = btn;
        home_domain_btn_labels[i] = lbl;
        lv_obj_add_event_cb(btn, domain_pill_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    update_domain_buttons(); /* Apply initial active highlight */

    /* === ITEMS CONTAINER: categorized, scrollable y=163 h=279 === */
    home_items_cont = lv_obj_create(scr_home);
    lv_obj_remove_style_all(home_items_cont);
    lv_obj_set_size(home_items_cont, LV_PCT(100), 279);
    lv_obj_set_flex_flow(home_items_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(home_items_cont, LEFT_SAFE, 0);
    lv_obj_set_style_pad_right(home_items_cont, RIGHT_SAFE, 0);
    lv_obj_set_style_pad_top(home_items_cont, 4, 0);
    lv_obj_set_style_pad_row(home_items_cont, 3, 0);
    lv_obj_align(home_items_cont, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 134);
    lv_obj_add_flag(home_items_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(home_items_cont, LV_DIR_VER);

    /* === ACTION BAR: RECORD | LISTEN | READ ALL  y=442 h=60 === */
    lv_obj_t *action_bar = lv_obj_create(scr_home);
    lv_obj_remove_style_all(action_bar);
    lv_obj_set_size(action_bar, LV_PCT(100), 60);
    lv_obj_set_flex_flow(action_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(action_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(action_bar, 0, 0);
    lv_obj_set_style_bg_color(action_bar, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_bg_opa(action_bar, LV_OPA_COVER, 0);
    lv_obj_align(action_bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* VOICE button — unified record + query (single large button) */
    home_record_btn = make_action_btn(action_bar, "● VOICE", COLOR_CORAL);
    home_record_btn_label = lv_obj_get_child(home_record_btn, 0);
    /* VIEW ALL button */
    home_read_btn = make_action_btn(action_bar, "VIEW ALL", COLOR_PURPLE);
    home_listen_btn = NULL;  /* merged into VOICE */

    lv_obj_add_event_cb(home_record_btn, record_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(home_read_btn,   read_btn_cb,   LV_EVENT_CLICKED, NULL);
}

static void create_screen_recording(void)
{
    scr_recording = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_recording, COLOR_BG, 0);

    /* Battery icon top-right */
    rec_batt_icon = lv_label_create(scr_recording);
    lv_label_set_text(rec_batt_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(rec_batt_icon, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(rec_batt_icon, &lv_font_montserrat_16, 0);
    lv_obj_align(rec_batt_icon, LV_ALIGN_TOP_RIGHT, -RIGHT_SAFE, TOP_SAFE);

    /* WiFi icon — left of battery */
    rec_wifi_icon = lv_label_create(scr_recording);
    lv_label_set_text(rec_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(rec_wifi_icon, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(rec_wifi_icon, &lv_font_montserrat_16, 0);
    lv_obj_align_to(rec_wifi_icon, rec_batt_icon, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    /* Recording indicator — simple red circle + REC text (no symbols that glitch) */
    lv_obj_t *rec_dot = lv_obj_create(scr_recording);
    lv_obj_remove_style_all(rec_dot);
    lv_obj_set_size(rec_dot, 20, 20);
    lv_obj_set_style_bg_color(rec_dot, COLOR_CORAL, 0);
    lv_obj_set_style_bg_opa(rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(rec_dot, LV_ALIGN_CENTER, -50, -90);

    lv_obj_t *rec_icon = lv_label_create(scr_recording);
    lv_label_set_text(rec_icon, "REC");
    lv_obj_set_style_text_color(rec_icon, COLOR_CORAL, 0);
    lv_obj_set_style_text_font(rec_icon, &lv_font_montserrat_36, 0);
    lv_obj_align(rec_icon, LV_ALIGN_CENTER, 10, -90);

    /* Waveform bars */
    lv_obj_t *bar_cont = lv_obj_create(scr_recording);
    lv_obj_remove_style_all(bar_cont);
    lv_obj_set_size(bar_cont, 120, 48);
    lv_obj_set_flex_flow(bar_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar_cont, 8, 0);
    lv_obj_align(bar_cont, LV_ALIGN_CENTER, 0, -20);

    for (int i = 0; i < 5; i++) {
        rec_bars[i] = lv_bar_create(bar_cont);
        lv_obj_set_size(rec_bars[i], 8, 48);
        lv_bar_set_range(rec_bars[i], 0, 100);
        lv_bar_set_value(rec_bars[i], 20, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(rec_bars[i], COLOR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(rec_bars[i], COLOR_CORAL, LV_PART_INDICATOR);
        lv_obj_set_style_radius(rec_bars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(rec_bars[i], 4, LV_PART_INDICATOR);
    }

    /* "Listening..." */
    lv_obj_t *lbl = lv_label_create(scr_recording);
    lv_label_set_text(lbl, "Listening...");
    lv_obj_set_style_text_color(lbl, COLOR_CORAL, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 30);

    /* Timer */
    rec_timer_label = lv_label_create(scr_recording);
    lv_label_set_text(rec_timer_label, "0:00");
    lv_obj_set_style_text_color(rec_timer_label, COLOR_DIM, 0);
    lv_obj_set_style_text_font(rec_timer_label, &lv_font_montserrat_20, 0);
    lv_obj_align(rec_timer_label, LV_ALIGN_CENTER, 0, 65);

    /* "Press button to stop" hint — updatable */
    rec_hint_label = lv_label_create(scr_recording);
    lv_label_set_text(rec_hint_label, "press to stop");
    lv_obj_set_style_text_color(rec_hint_label, COLOR_DIM, 0);
    lv_obj_set_style_text_font(rec_hint_label, &lv_font_montserrat_16, 0);
    lv_obj_align(rec_hint_label, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void create_screen_processing(void)
{
    scr_processing = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_processing, COLOR_BG, 0);

    /* Pulsing dots */
    proc_dots_label = lv_label_create(scr_processing);
    lv_label_set_text(proc_dots_label, ". . .");
    lv_obj_set_style_text_color(proc_dots_label, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(proc_dots_label, &lv_font_montserrat_48, 0);
    lv_obj_align(proc_dots_label, LV_ALIGN_CENTER, 0, -80);

    /* Title label — "Processing" or "Listening..." */
    proc_title_label = lv_label_create(scr_processing);
    lv_label_set_text(proc_title_label, "Processing");
    lv_obj_set_style_text_color(proc_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(proc_title_label, &lv_font_montserrat_28, 0);
    lv_obj_align(proc_title_label, LV_ALIGN_CENTER, 0, -20);

    /* Speaker playback indicator — teal circle with speaker icon, hidden by default */
    proc_speaker_bg = lv_obj_create(scr_processing);
    lv_obj_remove_style_all(proc_speaker_bg);
    lv_obj_set_size(proc_speaker_bg, 80, 80);
    lv_obj_set_style_bg_color(proc_speaker_bg, COLOR_TEAL, 0);
    lv_obj_set_style_bg_opa(proc_speaker_bg, LV_OPA_20, 0);
    lv_obj_set_style_radius(proc_speaker_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(proc_speaker_bg, LV_ALIGN_CENTER, 0, -60);
    lv_obj_add_flag(proc_speaker_bg, LV_OBJ_FLAG_HIDDEN);

    proc_speaker_icon = lv_label_create(proc_speaker_bg);
    lv_label_set_text(proc_speaker_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(proc_speaker_icon, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(proc_speaker_icon, &lv_font_montserrat_36, 0);
    lv_obj_center(proc_speaker_icon);

    /* Answer label — kept for fallback/error text, hidden by default */
    proc_answer_label = lv_label_create(scr_processing);
    lv_label_set_text(proc_answer_label, "");
    lv_obj_set_style_text_color(proc_answer_label, COLOR_DIM, 0);
    lv_obj_set_style_text_font(proc_answer_label, &lv_font_montserrat_16, 0);
    lv_obj_set_width(proc_answer_label, LV_PCT(85));
    lv_label_set_long_mode(proc_answer_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(proc_answer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(proc_answer_label, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(proc_answer_label, LV_OBJ_FLAG_HIDDEN);

    /* STT progress row */
    proc_stt_row = lv_obj_create(scr_processing);
    lv_obj_t *stt_row = proc_stt_row;
    lv_obj_remove_style_all(stt_row);
    lv_obj_set_size(stt_row, 300, 30);
    lv_obj_set_flex_flow(stt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stt_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stt_row, 12, 0);
    lv_obj_set_style_pad_left(stt_row, 20, 0);
    lv_obj_align(stt_row, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *stt_lbl = lv_label_create(stt_row);
    lv_label_set_text(stt_lbl, "STT");
    lv_obj_set_style_text_color(stt_lbl, COLOR_DIM, 0);
    lv_obj_set_style_text_font(stt_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(stt_lbl, 60);

    proc_stt_bar = lv_bar_create(stt_row);
    lv_obj_set_size(proc_stt_bar, 200, 8);
    lv_bar_set_range(proc_stt_bar, 0, 100);
    lv_bar_set_value(proc_stt_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(proc_stt_bar, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(proc_stt_bar, COLOR_TEAL, LV_PART_INDICATOR);
    lv_obj_set_style_radius(proc_stt_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(proc_stt_bar, 4, LV_PART_INDICATOR);

    /* Intent progress row — with clear gap below STT */
    proc_int_row = lv_obj_create(scr_processing);
    lv_obj_t *int_row = proc_int_row;
    lv_obj_remove_style_all(int_row);
    lv_obj_set_size(int_row, 300, 30);
    lv_obj_set_flex_flow(int_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(int_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(int_row, 12, 0);
    lv_obj_set_style_pad_left(int_row, 20, 0);
    lv_obj_align(int_row, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *int_lbl = lv_label_create(int_row);
    lv_label_set_text(int_lbl, "Intent");
    lv_obj_set_style_text_color(int_lbl, COLOR_DIM, 0);
    lv_obj_set_style_text_font(int_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(int_lbl, 60);

    proc_intent_bar = lv_bar_create(int_row);
    lv_obj_set_size(proc_intent_bar, 200, 8);
    lv_bar_set_range(proc_intent_bar, 0, 100);
    lv_bar_set_value(proc_intent_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(proc_intent_bar, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(proc_intent_bar, COLOR_PURPLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(proc_intent_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(proc_intent_bar, 4, LV_PART_INDICATOR);
}

static void create_screen_saved(void)
{
    scr_saved = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_saved, COLOR_BG, 0);

    /* Check circle */
    lv_obj_t *check = lv_obj_create(scr_saved);
    lv_obj_remove_style_all(check);
    lv_obj_set_size(check, 64, 64);
    lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(check, COLOR_TEAL, 0);
    lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
    lv_obj_align(check, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *check_lbl = lv_label_create(check);
    lv_label_set_text(check_lbl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(check_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(check_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(check_lbl);

    /* Result card */
    lv_obj_t *card = lv_obj_create(scr_saved);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(85), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 20);

    saved_badge_label = lv_label_create(card);
    lv_label_set_text(saved_badge_label, "REMINDER");
    lv_obj_set_style_text_font(saved_badge_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(saved_badge_label, lv_color_white(), 0);

    saved_title_label = lv_label_create(card);
    lv_label_set_text(saved_title_label, "");
    lv_obj_set_style_text_font(saved_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(saved_title_label, lv_color_white(), 0);
    lv_label_set_long_mode(saved_title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(saved_title_label, LV_PCT(100));

    saved_date_label = lv_label_create(card);
    lv_label_set_text(saved_date_label, "");
    lv_obj_set_style_text_color(saved_date_label, COLOR_DIM, 0);
    lv_obj_set_style_text_font(saved_date_label, &lv_font_montserrat_16, 0);

    saved_sync_label = lv_label_create(card);
    lv_label_set_text(saved_sync_label, "");
    lv_obj_set_style_text_color(saved_sync_label, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(saved_sync_label, &lv_font_montserrat_16, 0);
}

/* Today screen items container (populated dynamically) */
static lv_obj_t *today_items_cont = NULL;

static void create_screen_today(void)
{
    scr_today = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_today, COLOR_BG, 0);

    /* Scrollable items list — created FIRST so it's behind the header */
    today_items_cont = lv_obj_create(scr_today);
    lv_obj_remove_style_all(today_items_cont);
    lv_obj_set_size(today_items_cont, LV_PCT(100), 430);
    lv_obj_set_flex_flow(today_items_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(today_items_cont, 14, 0);
    lv_obj_set_style_pad_right(today_items_cont, 14, 0);
    lv_obj_set_style_pad_top(today_items_cont, 4, 0);
    lv_obj_set_style_pad_row(today_items_cont, 6, 0);
    lv_obj_add_flag(today_items_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(today_items_cont, LV_DIR_VER);
    lv_obj_align(today_items_cont, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 44);

    /* Back button — created AFTER items so it's on top (z-order) */
    lv_obj_t *back = lv_label_create(scr_today);
    lv_label_set_text(back, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back, COLOR_PURPLE, 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, LEFT_SAFE, TOP_SAFE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 30);
    lv_obj_add_event_cb(back, today_back_cb, LV_EVENT_CLICKED, NULL);

    /* Title */
    lv_obj_t *title = lv_label_create(scr_today);
    lv_label_set_text(title, "All Items");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TOP_SAFE);
}

static void create_screen_offline(void)
{
    scr_offline = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_offline, COLOR_BG, 0);

    lv_obj_t *wifi = lv_label_create(scr_offline);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi, COLOR_CORAL, 0);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_16, 0);
    lv_obj_align(wifi, LV_ALIGN_TOP_RIGHT, -RIGHT_SAFE, TOP_SAFE);

    lv_obj_t *banner = lv_obj_create(scr_offline);
    lv_obj_remove_style_all(banner);
    lv_obj_set_size(banner, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0x2A1A0A), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(banner, COLOR_AMBER, 0);
    lv_obj_set_style_border_width(banner, 1, 0);
    lv_obj_set_style_radius(banner, 8, 0);
    lv_obj_set_style_pad_all(banner, 12, 0);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 32);

    lv_obj_t *blbl = lv_label_create(banner);
    lv_label_set_text(blbl, LV_SYMBOL_WARNING " No internet\nRecordings queued on SD");
    lv_obj_set_style_text_color(blbl, COLOR_AMBER, 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(blbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(blbl, LV_PCT(100));

    offline_count_label = lv_label_create(scr_offline);
    lv_label_set_text(offline_count_label, "0 recordings pending");
    lv_obj_set_style_text_color(offline_count_label, COLOR_DIM, 0);
    lv_obj_set_style_text_font(offline_count_label, &lv_font_montserrat_20, 0);
    lv_obj_align(offline_count_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *space = lv_label_create(scr_offline);
    lv_label_set_text(space, "SD: checking...");
    lv_obj_set_style_text_color(space, COLOR_DIM, 0);
    lv_obj_set_style_text_font(space, &lv_font_montserrat_16, 0);
    lv_obj_align(space, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/* Domain badge click — kept for backward compat but pill buttons are primary */
static void __attribute__((unused)) domain_badge_click_cb(lv_event_t *e)
{
    (void)e;
    /* No-op: domain selection now via pill buttons */
}

/* Update pill button highlights to reflect s_current_domain */
static void update_domain_buttons(void)
{
    for (int i = 0; i < UI_DOMAIN_COUNT; i++) {
        if (!home_domain_btns[i]) continue;
        bool active = (i == (int)s_current_domain);
        lv_obj_set_style_bg_color(home_domain_btns[i],
            active ? COLOR_PURPLE : lv_color_hex(0x1E1E1E), 0);
        lv_obj_set_style_radius(home_domain_btns[i], 8, 0);
        if (home_domain_btn_labels[i]) {
            lv_obj_set_style_text_color(home_domain_btn_labels[i],
                active ? lv_color_white() : COLOR_DIM, 0);
        }
    }
}

/* Domain pill button — selects domain i */
static void domain_pill_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= UI_DOMAIN_COUNT) return;
    s_current_domain = (ui_domain_t)idx;
    update_domain_buttons();
    ESP_LOGI(TAG, "Domain set to: %s", DOMAIN_INFO[s_current_domain].id);
}

/* Action bar callbacks — invoke registered main.c handlers */
static void record_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_record_cb) s_record_cb();
}

static void __attribute__((unused)) listen_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_listen_cb) s_listen_cb();
}

static void read_btn_cb(lv_event_t *e)
{
    (void)e;
    /* VIEW ALL — show the All Items screen */
    if (s_read_all_cb) s_read_all_cb();
    else ui_show_screen_nolock(UI_SCREEN_TODAY);
}

/* Session screen: "Home" button callback */
static void session_home_cb(lv_event_t *e)
{
    (void)e;
    ui_show_screen_nolock(UI_SCREEN_HOME);
}

/* Session screen: "All Items" button callback */
static void session_all_cb(lv_event_t *e)
{
    (void)e;
    ui_show_screen_nolock(UI_SCREEN_TODAY);
}

static void create_screen_session(void)
{
    scr_session = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_session, COLOR_BG, 0);

    /* Header label */
    session_header_label = lv_label_create(scr_session);
    lv_label_set_text(session_header_label, "Session Summary");
    lv_obj_set_style_text_font(session_header_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(session_header_label, lv_color_white(), 0);
    lv_obj_align(session_header_label, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 4);

    /* Scrollable items container */
    session_items_cont = lv_obj_create(scr_session);
    lv_obj_remove_style_all(session_items_cont);
    lv_obj_set_size(session_items_cont, LV_PCT(100), 390);
    lv_obj_set_flex_flow(session_items_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(session_items_cont, LEFT_SAFE, 0);
    lv_obj_set_style_pad_right(session_items_cont, RIGHT_SAFE, 0);
    lv_obj_set_style_pad_top(session_items_cont, 4, 0);
    lv_obj_set_style_pad_row(session_items_cont, 4, 0);
    lv_obj_add_flag(session_items_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(session_items_cont, LV_DIR_VER);
    lv_obj_align(session_items_cont, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 44);

    /* Bottom nav row */
    lv_obj_t *nav = lv_obj_create(scr_session);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 52);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 24, 0);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *all_btn = lv_label_create(nav);
    lv_label_set_text(all_btn, "View All");
    lv_obj_set_style_text_color(all_btn, COLOR_PURPLE, 0);
    lv_obj_set_style_text_font(all_btn, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(all_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(all_btn, 20);
    lv_obj_add_event_cb(all_btn, session_all_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_btn = lv_label_create(nav);
    lv_label_set_text(home_btn, "Home");
    lv_obj_set_style_text_color(home_btn, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(home_btn, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(home_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(home_btn, 20);
    lv_obj_add_event_cb(home_btn, session_home_cb, LV_EVENT_CLICKED, NULL);
}

static void create_screen_finished(void)
{
    scr_finished = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_finished, COLOR_BG, 0);

    /* Scrollable items list — created FIRST so back button is on top */
    finished_items_cont = lv_obj_create(scr_finished);
    lv_obj_remove_style_all(finished_items_cont);
    lv_obj_set_size(finished_items_cont, LV_PCT(100), 430);
    lv_obj_set_flex_flow(finished_items_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(finished_items_cont, 14, 0);
    lv_obj_set_style_pad_right(finished_items_cont, 14, 0);
    lv_obj_set_style_pad_top(finished_items_cont, 4, 0);
    lv_obj_set_style_pad_row(finished_items_cont, 6, 0);
    lv_obj_add_flag(finished_items_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(finished_items_cont, LV_DIR_VER);
    lv_obj_align(finished_items_cont, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 44);

    /* Back button — on top of items (z-order) */
    lv_obj_t *back = lv_label_create(scr_finished);
    lv_label_set_text(back, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back, COLOR_PURPLE, 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, LEFT_SAFE, TOP_SAFE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 30);
    lv_obj_add_event_cb(back, finished_back_cb, LV_EVENT_CLICKED, NULL);

    /* Title */
    lv_obj_t *title = lv_label_create(scr_finished);
    lv_label_set_text(title, "Finished");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TOP_SAFE);
}

/* ---------- Build an item row for home or today screen ---------- */

static lv_obj_t *create_item_row(lv_obj_t *parent, int index, bool show_actions)
{
    if (index < 0 || index >= s_items_count) return NULL;

    /* Outer wrapper — use ROW layout: [color bar | text column]
     * No nested container — keeps the bar and text side by side. */
    lv_obj_t *wrapper = lv_obj_create(parent);
    lv_obj_remove_style_all(wrapper);
    lv_obj_set_width(wrapper, LV_PCT(100));
    lv_obj_set_height(wrapper, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(wrapper, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(wrapper, LV_OPA_60, 0);
    lv_obj_set_style_radius(wrapper, 10, 0);
    lv_obj_set_style_pad_ver(wrapper, 8, 0);
    lv_obj_set_style_pad_hor(wrapper, 10, 0);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrapper, 10, 0);

    /* Color bar (thin vertical line) — fixed size, no flex grow */
    lv_obj_t *bar = lv_obj_create(wrapper);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 4, 36);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, type_color(s_item_types[index]), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);  /* clicks bubble to wrapper */

    /* Text column — takes remaining space */
    lv_obj_t *col = lv_obj_create(wrapper);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);  /* clicks bubble to wrapper */

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, s_item_titles[index]);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Date + paused indicator on same row */
    lv_obj_t *date_row = lv_obj_create(col);
    lv_obj_remove_style_all(date_row);
    lv_obj_set_width(date_row, LV_PCT(100));
    lv_obj_set_height(date_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(date_row, 6, 0);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(date_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *m = lv_label_create(date_row);
    lv_label_set_text(m, s_item_dates[index]);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(m, COLOR_DIM, 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Paused badge */
    if (s_item_paused[index]) {
        lv_obj_t *pause_badge = lv_label_create(date_row);
        lv_label_set_text(pause_badge, LV_SYMBOL_PAUSE " Paused");
        lv_obj_set_style_text_font(pause_badge, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(pause_badge, COLOR_AMBER, 0);
        lv_obj_add_flag(pause_badge, LV_OBJ_FLAG_EVENT_BUBBLE);

        /* Dim the entire card when paused */
        lv_obj_set_style_opa(wrapper, LV_OPA_60, 0);
    }

    /* Tap-to-reveal action buttons (Today screen only) */
    if (show_actions && strlen(s_item_ids[index]) > 0) {
        /* Action row — added to parent container, initially hidden */
        lv_obj_t *btn_row = lv_obj_create(parent);
        lv_obj_remove_style_all(btn_row);
        lv_obj_set_width(btn_row, LV_PCT(100));
        lv_obj_set_height(btn_row, 40);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(btn_row, LV_OBJ_FLAG_HIDDEN);

        /* Register tap to toggle action buttons */
        if (s_tap_pool_idx < MAX_TAP_DATA) {
            tap_toggle_data_t *td = &s_tap_pool[s_tap_pool_idx++];
            td->btn_row = btn_row;
            td->revealed = false;
            lv_obj_add_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(wrapper, item_tap_cb, LV_EVENT_CLICKED, td);
        }

        /* Done button */
        lv_obj_t *done_btn = lv_obj_create(btn_row);
        lv_obj_remove_style_all(done_btn);
        lv_obj_set_size(done_btn, 90, 32);
        lv_obj_set_style_bg_color(done_btn, COLOR_TEAL, 0);
        lv_obj_set_style_bg_opa(done_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(done_btn, 6, 0);
        lv_obj_add_flag(done_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(done_btn, item_action_event_cb, LV_EVENT_CLICKED,
                           alloc_action_data(index, "done"));
        lv_obj_t *done_lbl = lv_label_create(done_btn);
        lv_label_set_text(done_lbl, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(done_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(done_lbl);

        /* Pause button */
        lv_obj_t *pause_btn = lv_obj_create(btn_row);
        lv_obj_remove_style_all(pause_btn);
        lv_obj_set_size(pause_btn, 90, 32);
        lv_obj_set_style_bg_color(pause_btn, COLOR_AMBER, 0);
        lv_obj_set_style_bg_opa(pause_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pause_btn, 6, 0);
        lv_obj_add_flag(pause_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pause_btn, item_action_event_cb, LV_EVENT_CLICKED,
                           alloc_action_data(index, "pause"));
        lv_obj_t *pause_lbl = lv_label_create(pause_btn);
        lv_label_set_text(pause_lbl, s_item_paused[index]
            ? LV_SYMBOL_PLAY " Resume"
            : LV_SYMBOL_PAUSE " Pause");
        lv_obj_set_style_text_color(pause_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(pause_lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(pause_lbl);

        /* Delete button */
        lv_obj_t *del_btn = lv_obj_create(btn_row);
        lv_obj_remove_style_all(del_btn);
        lv_obj_set_size(del_btn, 90, 32);
        lv_obj_set_style_bg_color(del_btn, COLOR_CORAL, 0);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(del_btn, 6, 0);
        lv_obj_add_flag(del_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(del_btn, item_action_event_cb, LV_EVENT_CLICKED,
                           alloc_action_data(index, "delete"));
        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(del_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(del_lbl);
    }

    return wrapper;
}

/* ---------- Provisioning screen (created dynamically) ---------- */

static lv_obj_t *scr_provision = NULL;

/* Animation-safe wrapper: lv_obj_set_style_opa takes 3 args, anim cb passes 2 */
static void anim_set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void ui_show_provisioning_screen(const char *ap_ssid)
{
    ui_lock();

    if (scr_provision) {
        lv_obj_del(scr_provision);
    }
    scr_provision = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_provision, COLOR_BG, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(scr_provision);
    lv_label_set_text(title, "Setup Required");
    lv_obj_set_style_text_color(title, COLOR_TEAL, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 4);

    /* WiFi icon in circle */
    lv_obj_t *icon_bg = lv_obj_create(scr_provision);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, 56, 56);
    lv_obj_set_style_radius(icon_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon_bg, lv_color_hex(0x0F6E56), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_align(icon_bg, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 38);

    lv_obj_t *wifi_icon = lv_label_create(icon_bg);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(wifi_icon);

    /* Step 1: Connect phone to AP */
    lv_obj_t *instr1 = lv_label_create(scr_provision);
    lv_label_set_text(instr1, "1. Connect your phone to:");
    lv_obj_set_style_text_color(instr1, COLOR_DIM, 0);
    lv_obj_set_style_text_font(instr1, &lv_font_montserrat_14, 0);
    lv_obj_align(instr1, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 104);

    /* SSID box */
    lv_obj_t *ssid_box = lv_obj_create(scr_provision);
    lv_obj_remove_style_all(ssid_box);
    lv_obj_set_size(ssid_box, 260, 40);
    lv_obj_set_style_bg_color(ssid_box, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(ssid_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ssid_box, COLOR_TEAL, 0);
    lv_obj_set_style_border_width(ssid_box, 1, 0);
    lv_obj_set_style_radius(ssid_box, 8, 0);
    lv_obj_align(ssid_box, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 124);

    lv_obj_t *ssid_lbl = lv_label_create(ssid_box);
    lv_label_set_text(ssid_lbl, ap_ssid);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(ssid_lbl);

    /* Step 2: Open browser */
    lv_obj_t *instr2 = lv_label_create(scr_provision);
    lv_label_set_text(instr2, "2. Open browser and go to:");
    lv_obj_set_style_text_color(instr2, COLOR_DIM, 0);
    lv_obj_set_style_text_font(instr2, &lv_font_montserrat_14, 0);
    lv_obj_align(instr2, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 180);

    lv_obj_t *url_lbl = lv_label_create(scr_provision);
    lv_label_set_text(url_lbl, "192.168.4.1");
    lv_obj_set_style_text_color(url_lbl, COLOR_PURPLE, 0);
    lv_obj_set_style_text_font(url_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(url_lbl, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 200);

    /* Step 3: Enter WiFi credentials */
    lv_obj_t *instr3 = lv_label_create(scr_provision);
    lv_label_set_text(instr3, "3. Enter your WiFi credentials");
    lv_obj_set_style_text_color(instr3, COLOR_DIM, 0);
    lv_obj_set_style_text_font(instr3, &lv_font_montserrat_14, 0);
    lv_obj_align(instr3, LV_ALIGN_TOP_MID, 0, TOP_SAFE + 240);

    /* Pulsing dot — shows device is waiting */
    lv_obj_t *dot = lv_obj_create(scr_provision);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, COLOR_TEAL, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dot);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 800);
    lv_anim_set_playback_time(&a, 800);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    /* "Waiting for setup..." */
    lv_obj_t *wait_lbl = lv_label_create(scr_provision);
    lv_label_set_text(wait_lbl, "Waiting for setup...");
    lv_obj_set_style_text_color(wait_lbl, COLOR_DIM, 0);
    lv_obj_set_style_text_font(wait_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(wait_lbl, LV_ALIGN_BOTTOM_MID, 0, -42);

    lv_scr_load(scr_provision);
    ui_unlock();
}

/* ---------- Public API ---------- */

esp_err_t ui_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateMutex();

    lv_init();

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);

    init_display_hw();

    s_display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    size_t buf_size = LCD_WIDTH * 60 * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Display buffers: %u bytes each in PSRAM (buf1=%p buf2=%p)",
             (unsigned)buf_size, buf1, buf2);
    lv_display_set_buffers(s_display, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, disp_flush_cb);

    init_touch_hw();

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);

    lv_theme_t *theme = lv_theme_default_init(
        s_display,
        COLOR_PURPLE,
        COLOR_TEAL,
        true,
        &lv_font_montserrat_16
    );
    lv_display_set_theme(s_display, theme);

    create_screen_home();
    create_screen_recording();
    create_screen_processing();
    create_screen_saved();
    create_screen_today();
    create_screen_offline();
    create_screen_finished();
    create_screen_session();

    /* Clock update timer — every 1 second */
    lv_timer_create(clock_timer_cb, 1000, NULL);

    lv_screen_load(scr_home);

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "UI initialized: %dx%d, 8 screens created", LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

void ui_show_screen(ui_screen_t screen)
{
    lv_obj_t *screens[] = {
        scr_home, scr_recording, scr_processing,
        scr_saved, scr_today, scr_offline, scr_finished, scr_session,
    };
    if (screen >= sizeof(screens) / sizeof(screens[0])) return;
    ui_lock();
    lv_screen_load_anim(screens[screen], LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    ui_unlock();
}

void ui_show_screen_nolock(ui_screen_t screen)
{
    lv_obj_t *screens[] = {
        scr_home, scr_recording, scr_processing,
        scr_saved, scr_today, scr_offline, scr_finished, scr_session,
    };
    if (screen >= sizeof(screens) / sizeof(screens[0])) return;
    lv_screen_load_anim(screens[screen], LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

void ui_set_connected(bool connected)
{
    ui_lock();
    lv_color_t color = connected ? COLOR_TEAL : COLOR_CORAL;
    if (home_wifi_icon) lv_obj_set_style_text_color(home_wifi_icon, color, 0);
    if (rec_wifi_icon)  lv_obj_set_style_text_color(rec_wifi_icon, color, 0);
    ui_unlock();
}

void ui_set_battery(int pct, bool charging)
{
    const char *icon;
    lv_color_t color;

    if (charging) {
        icon = LV_SYMBOL_CHARGE;
        color = COLOR_TEAL;
    } else if (pct >= 80) {
        icon = LV_SYMBOL_BATTERY_FULL;
        color = COLOR_TEAL;
    } else if (pct >= 60) {
        icon = LV_SYMBOL_BATTERY_3;
        color = COLOR_TEAL;
    } else if (pct >= 40) {
        icon = LV_SYMBOL_BATTERY_2;
        color = COLOR_TEAL;
    } else if (pct >= 20) {
        icon = LV_SYMBOL_BATTERY_1;
        color = lv_color_hex(0xE8A317);  /* amber */
    } else {
        icon = LV_SYMBOL_BATTERY_EMPTY;
        color = COLOR_CORAL;  /* red */
    }

    ui_lock();
    if (home_batt_icon) {
        lv_label_set_text(home_batt_icon, icon);
        lv_obj_set_style_text_color(home_batt_icon, color, 0);
    }
    if (rec_batt_icon) {
        lv_label_set_text(rec_batt_icon, icon);
        lv_obj_set_style_text_color(rec_batt_icon, color, 0);
    }
    ui_unlock();
}

void ui_update_waveform(uint16_t rms)
{
    ui_lock();
    int base = (rms * 100) / 32767;
    if (base > 100) base = 100;
    for (int i = 0; i < 5; i++) {
        int val = base + ((i * 7 + rms) % 20) - 10;
        if (val < 5) val = 5;
        if (val > 100) val = 100;
        lv_bar_set_value(rec_bars[i], val, LV_ANIM_ON);
    }
    ui_unlock();
}

void ui_set_rec_timer(uint32_t seconds)
{
    ui_lock();
    if (rec_timer_label) {
        lv_label_set_text_fmt(rec_timer_label, "%lu:%02lu",
                              (unsigned long)(seconds / 60),
                              (unsigned long)(seconds % 60));
    }
    ui_unlock();
}

void ui_set_recording_hint(const char *text)
{
    ui_lock();
    if (rec_hint_label) lv_label_set_text(rec_hint_label, text ? text : "press to stop");
    ui_unlock();
}

void ui_set_progress(int stt_pct, int intent_pct)
{
    ui_lock();
    if (proc_stt_bar)    lv_bar_set_value(proc_stt_bar, stt_pct, LV_ANIM_ON);
    if (proc_intent_bar) lv_bar_set_value(proc_intent_bar, intent_pct, LV_ANIM_ON);
    ui_unlock();
}

void ui_set_processing_mode(bool query_mode)
{
    ui_lock();
    /* Always reset speaker icon and dots when switching modes */
    if (proc_speaker_bg)  lv_obj_add_flag(proc_speaker_bg, LV_OBJ_FLAG_HIDDEN);
    if (proc_dots_label)  lv_obj_remove_flag(proc_dots_label, LV_OBJ_FLAG_HIDDEN);
    if (proc_answer_label) lv_obj_add_flag(proc_answer_label, LV_OBJ_FLAG_HIDDEN);

    if (query_mode) {
        /* Query/Ask mode — hide STT/Intent bars, show "Thinking..." */
        if (proc_stt_row)     lv_obj_add_flag(proc_stt_row, LV_OBJ_FLAG_HIDDEN);
        if (proc_int_row)     lv_obj_add_flag(proc_int_row, LV_OBJ_FLAG_HIDDEN);
        if (proc_title_label) lv_label_set_text(proc_title_label, "Thinking...");
        if (proc_dots_label)  lv_obj_set_style_text_color(proc_dots_label, COLOR_PURPLE, 0);
    } else {
        /* Ingest mode — show STT/Intent bars */
        if (proc_stt_row)     lv_obj_remove_flag(proc_stt_row, LV_OBJ_FLAG_HIDDEN);
        if (proc_int_row)     lv_obj_remove_flag(proc_int_row, LV_OBJ_FLAG_HIDDEN);
        if (proc_title_label) lv_label_set_text(proc_title_label, "Processing");
        if (proc_dots_label)  lv_obj_set_style_text_color(proc_dots_label, COLOR_TEAL, 0);
        /* Reset progress bars */
        if (proc_stt_bar)    lv_bar_set_value(proc_stt_bar, 0, LV_ANIM_OFF);
        if (proc_intent_bar) lv_bar_set_value(proc_intent_bar, 0, LV_ANIM_OFF);
    }
    ui_unlock();
}

void ui_show_query_answer(const char *text)
{
    ui_lock();
    /* Hide dots and STT/Intent bars */
    if (proc_dots_label)  lv_obj_add_flag(proc_dots_label, LV_OBJ_FLAG_HIDDEN);
    if (proc_stt_row)     lv_obj_add_flag(proc_stt_row, LV_OBJ_FLAG_HIDDEN);
    if (proc_int_row)     lv_obj_add_flag(proc_int_row, LV_OBJ_FLAG_HIDDEN);

    /* Show speaker icon */
    if (proc_speaker_bg)  lv_obj_remove_flag(proc_speaker_bg, LV_OBJ_FLAG_HIDDEN);
    if (proc_title_label) lv_label_set_text(proc_title_label, "");

    /* Show error/fallback text only if it's not a normal playback */
    if (proc_answer_label) {
        if (text && strstr(text, "failed")) {
            lv_label_set_text(proc_answer_label, text);
            lv_obj_remove_flag(proc_answer_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(proc_answer_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_unlock();
}

void ui_show_saved_item(const char *type, const char *title,
                        const char *date, float confidence)
{
    ui_lock();
    if (saved_badge_label) {
        char upper[16];
        strncpy(upper, type, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (char *p = upper; *p; p++) *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
        lv_label_set_text(saved_badge_label, upper);
        lv_obj_set_style_text_color(saved_badge_label, type_color(type), 0);
    }
    if (saved_title_label) lv_label_set_text(saved_title_label, title);
    if (saved_date_label)  lv_label_set_text(saved_date_label, date ? date : "");
    if (saved_sync_label) {
        lv_label_set_text(saved_sync_label,
                          confidence >= 0.6f ? LV_SYMBOL_OK " Saved" : LV_SYMBOL_WARNING " Needs confirmation");
        lv_obj_set_style_text_color(saved_sync_label,
                                     confidence >= 0.6f ? COLOR_TEAL : COLOR_AMBER, 0);
    }
    ui_unlock();
}

void ui_set_home_items(const ui_item_t *items, int count)
{
    ui_lock();
    if (!home_items_cont) { ui_unlock(); return; }

    s_items_count = count > MAX_ITEMS ? MAX_ITEMS : count;
    s_action_pool_idx = 0;
    s_tap_pool_idx = 0;
    s_open_row = NULL;

    for (int i = 0; i < s_items_count; i++) {
        strncpy(s_item_types[i], items[i].type ? items[i].type : "note",
                sizeof(s_item_types[i]) - 1);
        s_item_types[i][sizeof(s_item_types[i]) - 1] = '\0';

        strncpy(s_item_categories[i],
                items[i].category && items[i].category[0] ? items[i].category : s_item_types[i],
                sizeof(s_item_categories[i]) - 1);
        s_item_categories[i][sizeof(s_item_categories[i]) - 1] = '\0';

        strncpy(s_item_titles[i], items[i].title ? items[i].title : "",
                sizeof(s_item_titles[i]) - 1);
        s_item_titles[i][sizeof(s_item_titles[i]) - 1] = '\0';

        /* Date: prefer formatted date; fallback to recurrence */
        if (items[i].date && items[i].date[0]) {
            strncpy(s_item_dates[i], items[i].date, sizeof(s_item_dates[i]) - 1);
        } else if (items[i].recurrence && items[i].recurrence[0]) {
            snprintf(s_item_dates[i], sizeof(s_item_dates[i]),
                     LV_SYMBOL_REFRESH " %s", items[i].recurrence);
        } else {
            s_item_dates[i][0] = '\0';
        }
        s_item_dates[i][sizeof(s_item_dates[i]) - 1] = '\0';

        strncpy(s_item_recurrences[i],
                items[i].recurrence ? items[i].recurrence : "",
                sizeof(s_item_recurrences[i]) - 1);
        s_item_recurrences[i][sizeof(s_item_recurrences[i]) - 1] = '\0';

        strncpy(s_item_ids[i], items[i].id ? items[i].id : "",
                sizeof(s_item_ids[i]) - 1);
        s_item_ids[i][sizeof(s_item_ids[i]) - 1] = '\0';

        s_item_paused[i] = items[i].paused;
        s_item_done[i]   = items[i].done;

        s_items_copy[i].type       = s_item_types[i];
        s_items_copy[i].category   = s_item_categories[i];
        s_items_copy[i].title      = s_item_titles[i];
        s_items_copy[i].date       = s_item_dates[i];
        s_items_copy[i].id         = s_item_ids[i];
        s_items_copy[i].paused     = items[i].paused;
        s_items_copy[i].done       = items[i].done;
    }

    /* === Rebuild home screen: grouped by category with ETA === */
    lv_obj_clean(home_items_cont);

    static const struct {
        const char *cat;
        const char *header;
        lv_color_t  color;
    } CATS[] = {
        { "insight", "INSIGHTS", { .red=0xEF, .green=0x9F, .blue=0x27 } }, /* amber */
        { "action",  "ACTIONS",  { .red=0x7F, .green=0x77, .blue=0xDD } }, /* purple */
        { "event",   "EVENTS",   { .red=0x1D, .green=0x9E, .blue=0x75 } }, /* teal */
        { "note",    "NOTES",    { .red=0x66, .green=0x66, .blue=0x66 } }, /* grey */
    };

    int total_active = 0;
    for (int i = 0; i < s_items_count; i++) {
        if (!s_item_done[i]) total_active++;
    }

    if (total_active == 0) {
        lv_obj_t *empty = lv_label_create(home_items_cont);
        lv_label_set_text(empty, "No items yet.\nTap RECORD to add one.");
        lv_obj_set_style_text_color(empty, COLOR_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_top(empty, 40, 0);
    } else {
        for (int c = 0; c < 4; c++) {
            bool header_shown = false;
            lv_color_t col = lv_color_make(CATS[c].color.red,
                                            CATS[c].color.green,
                                            CATS[c].color.blue);
            for (int i = 0; i < s_items_count; i++) {
                if (s_item_done[i]) continue;
                if (strcmp(s_item_categories[i], CATS[c].cat) != 0) continue;

                /* Section header — shown once per category */
                if (!header_shown) {
                    lv_obj_t *hdr = lv_label_create(home_items_cont);
                    lv_label_set_text(hdr, CATS[c].header);
                    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(hdr, col, 0);
                    lv_obj_set_style_pad_top(hdr, 6, 0);
                    lv_obj_set_width(hdr, LV_PCT(100));
                    header_shown = true;
                }

                /* Item row: left color border | title | date right-aligned */
                lv_obj_t *row = lv_obj_create(home_items_cont);
                lv_obj_remove_style_all(row);
                lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
                lv_obj_set_style_min_height(row, 34, 0);
                lv_obj_set_style_bg_color(row, lv_color_hex(0x141414), 0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(row, 6, 0);
                lv_obj_set_style_border_color(row, col, 0);
                lv_obj_set_style_border_width(row, 2, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_pad_left(row, 8, 0);
                lv_obj_set_style_pad_right(row, 6, 0);
                lv_obj_set_style_pad_ver(row, 4, 0);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

                /* Title — takes remaining space */
                lv_obj_t *title = lv_label_create(row);
                lv_label_set_text(title, s_item_titles[i]);
                lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(title,
                    s_item_paused[i] ? COLOR_DIM : lv_color_white(), 0);
                lv_obj_set_flex_grow(title, 1);

                /* ETA date — right side, smaller, dim */
                if (s_item_dates[i][0]) {
                    lv_obj_t *date_lbl = lv_label_create(row);
                    lv_label_set_text(date_lbl, s_item_dates[i]);
                    lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(date_lbl, COLOR_DIM, 0);
                    lv_obj_set_style_pad_left(date_lbl, 4, 0);
                }
            }
        }
    }

    /* === Rebuild All Items screen (Today) — use category grouping too === */
    if (today_items_cont) {
        lv_obj_clean(today_items_cont);
        for (int c = 0; c < 4; c++) {
            lv_color_t col = lv_color_make(CATS[c].color.red,
                                            CATS[c].color.green,
                                            CATS[c].color.blue);
            bool header_shown = false;
            for (int i = 0; i < s_items_count; i++) {
                if (s_item_done[i]) continue;
                if (strcmp(s_item_categories[i], CATS[c].cat) != 0) continue;
                if (!header_shown) {
                    lv_obj_t *hdr = lv_label_create(today_items_cont);
                    lv_label_set_text(hdr, CATS[c].header);
                    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(hdr, col, 0);
                    lv_obj_set_style_pad_top(hdr, 4, 0);
                    header_shown = true;
                }
                create_item_row(today_items_cont, i, true);
            }
        }
    }

    /* === Rebuild finished screen === */
    if (finished_items_cont) {
        lv_obj_clean(finished_items_cont);
        for (int i = 0; i < s_items_count; i++) {
            if (s_item_done[i]) create_item_row(finished_items_cont, i, false);
        }
    }

    ui_unlock();
}

void ui_set_queue_count(int count)
{
    ui_lock();
    if (offline_count_label) {
        lv_label_set_text_fmt(offline_count_label, "%d recording%s pending",
                              count, count == 1 ? "" : "s");
    }
    ui_unlock();
}

void ui_set_item_action_cb(ui_item_action_cb_t cb)
{
    s_item_action_cb = cb;
}

/* ---------- Display power control (CO5300 AMOLED) ---------- */

void ui_display_sleep(void)
{
    ui_lock();
    co5300_send_cmd(0x28, NULL, 0);   /* Display Off */
    vTaskDelay(pdMS_TO_TICKS(20));
    co5300_send_cmd(0x10, NULL, 0);   /* Sleep In */
    ui_unlock();
    ESP_LOGI(TAG, "AMOLED sleep");
}

void ui_display_wake(void)
{
    ui_lock();
    co5300_send_cmd(0x11, NULL, 0);   /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));   /* Wait for panel to wake — datasheet says 120ms */
    co5300_send_cmd(0x29, NULL, 0);   /* Display On */
    ui_unlock();
    ESP_LOGI(TAG, "AMOLED wake");
}

/* ---------- Domain API ---------- */

const char *ui_get_domain_str(void)
{
    return DOMAIN_INFO[s_current_domain].id;
}

ui_domain_t ui_get_domain(void)
{
    return s_current_domain;
}

void ui_set_domain(ui_domain_t domain)
{
    if (domain >= UI_DOMAIN_COUNT) domain = UI_DOMAIN_GENERAL;
    s_current_domain = domain;
    ui_lock();
    update_domain_buttons();
    ui_unlock();
}

void ui_cycle_domain(void)
{
    ui_set_domain((ui_domain_t)((s_current_domain + 1) % UI_DOMAIN_COUNT));
    ESP_LOGI(TAG, "Domain: %s", DOMAIN_INFO[s_current_domain].id);
}

/* ---------- Action bar API ---------- */

void ui_set_recording(bool active)
{
    ui_lock();
    if (home_record_btn_label) {
        lv_label_set_text(home_record_btn_label, active ? "■ STOP" : "● VOICE");
        lv_obj_set_style_text_color(home_record_btn_label, active ? COLOR_AMBER : COLOR_CORAL, 0);
    }
    ui_unlock();
}

void ui_set_playing(bool playing)
{
    ui_lock();
    if (home_record_btn_label) {
        if (playing) {
            lv_label_set_text(home_record_btn_label, "■ STOP");
            lv_obj_set_style_text_color(home_record_btn_label, COLOR_TEAL, 0);
        } else {
            lv_label_set_text(home_record_btn_label, "● VOICE");
            lv_obj_set_style_text_color(home_record_btn_label, COLOR_CORAL, 0);
        }
    }
    ui_unlock();
}

void ui_set_record_cb(ui_action_cb_t cb)   { s_record_cb   = cb; }
void ui_set_listen_cb(ui_action_cb_t cb)   { s_listen_cb   = cb; }
void ui_set_read_all_cb(ui_action_cb_t cb) { s_read_all_cb = cb; }

/* ---------- Session Summary API ---------- */

void ui_set_session_items(const char *domain_str,
                          const ui_session_item_t *items, int count)
{
    ui_lock();

    /* Update header */
    if (session_header_label && domain_str) {
        /* Capitalize domain first letter for display */
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%s · %d items", domain_str, count);
        hdr[0] = (char)(hdr[0] >= 'a' && hdr[0] <= 'z' ? hdr[0] - 32 : hdr[0]);
        lv_label_set_text(session_header_label, hdr);
    }

    /* Clear existing items */
    if (session_items_cont) {
        lv_obj_clean(session_items_cont);
    } else {
        ui_unlock();
        return;
    }

    /* Section colors and labels */
    static const struct { const char *cat; const char *label; lv_color_t color; } CATS[] = {
        { "insight", "INSIGHTS", { .red = 0xEF, .green = 0x9F, .blue = 0x27 } },
        { "action",  "ACTIONS",  { .red = 0x7F, .green = 0x77, .blue = 0xDD } },
        { "event",   "EVENTS",   { .red = 0x1D, .green = 0x9E, .blue = 0x75 } },
        { "note",    "NOTES",    { .red = 0x66, .green = 0x66, .blue = 0x66 } },
    };

    for (int c = 0; c < 4; c++) {
        /* Check if any items belong to this category */
        bool has_items = false;
        for (int i = 0; i < count; i++) {
            const char *cat = items[i].category ? items[i].category : "note";
            if (strcmp(cat, CATS[c].cat) == 0) { has_items = true; break; }
        }
        if (!has_items) continue;

        /* Section header label */
        lv_obj_t *sec_lbl = lv_label_create(session_items_cont);
        lv_label_set_text(sec_lbl, CATS[c].label);
        lv_obj_set_style_text_font(sec_lbl, &lv_font_montserrat_14, 0);
        lv_color_t sec_color = lv_color_make(CATS[c].color.red, CATS[c].color.green, CATS[c].color.blue);
        lv_obj_set_style_text_color(sec_lbl, sec_color, 0);
        lv_obj_set_style_pad_top(sec_lbl, 6, 0);

        /* Items in this category */
        for (int i = 0; i < count; i++) {
            const char *cat = items[i].category ? items[i].category : "note";
            if (strcmp(cat, CATS[c].cat) != 0) continue;

            lv_obj_t *row = lv_obj_create(session_items_cont);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_border_color(row, sec_color, 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_pad_left(row, 8, 0);
            lv_obj_set_style_pad_ver(row, 2, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(row, 0, 0);

            lv_obj_t *title_lbl = lv_label_create(row);
            lv_label_set_text(title_lbl, items[i].title ? items[i].title : "");
            lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(title_lbl, LV_PCT(100));
            lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);

            /* Sub-label: date or recurrence */
            const char *meta = NULL;
            if (items[i].date && items[i].date[0]) meta = items[i].date;
            else if (items[i].recurrence && items[i].recurrence[0]) meta = items[i].recurrence;
            if (meta) {
                lv_obj_t *meta_lbl = lv_label_create(row);
                lv_label_set_text(meta_lbl, meta);
                lv_obj_set_style_text_font(meta_lbl, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(meta_lbl, COLOR_DIM, 0);
            }
        }
    }

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(session_items_cont);
        lv_label_set_text(empty, "No items extracted.");
        lv_obj_set_style_text_color(empty, COLOR_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    }

    ui_unlock();
}
