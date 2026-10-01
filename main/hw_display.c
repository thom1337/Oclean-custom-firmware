#include "hardware.h"
#include "metrics.h"
#include "mqtt_ha.h"
#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// ST7735S 80x160 RGB565 on SPI2 (recovered from stock FW).
// Pins: MOSI=40 SCLK=39 CS=38 DC=41 RST=42. Backlight = LEDC channel 4 on GPIO21,
// active-low (duty 0 = full brightness, 8191 = off) — RE-confirmed; it is already
// configured by hw_io_init, we just assert it on here and own it.
#define PIN_MOSI 40
#define PIN_SCLK 39
#define PIN_CS   38
#define PIN_DC   41
#define PIN_RST  42
#define LCD_W 80
#define LCD_H 160
#define COL_OFFSET 24
#define ROW_OFFSET 0
#define BL_CHANNEL LEDC_CHANNEL_4

#define RGB(r,g,b) ((uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)))
#define BLACK RGB(0,0,0)
#define WHITE RGB(235,238,245)
#define GREEN RGB(45,205,110)
#define RED   RGB(230,70,60)
#define BLUE  RGB(70,150,240)
#define GREY  RGB(70,80,92)
#define DIM   RGB(120,130,145)

static const char *TAG = "hw_lcd";
static spi_device_handle_t s_spi;

static void lcd_cmd(uint8_t c)
{
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    spi_device_polling_transmit(s_spi, &t);
}
static void lcd_data(const uint8_t *d, int n)
{
    if (n <= 0) return;
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = { .length = 8 * n, .tx_buffer = d };
    spi_device_polling_transmit(s_spi, &t);
}

// huaersheng_0305 + INVON init table (confirmed working on this unit: black
// renders black, shapes are visible). {cmd, nargs, args...}; cmd 0xFF,ms = delay.
static const uint8_t INIT[] = {
    0x11,0,
    0xFF,120,
    0xB1,3, 0x05,0x3A,0x3A,
    0xB2,3, 0x05,0x3A,0x3A,
    0xB3,6, 0x05,0x3A,0x3A,0x05,0x3A,0x3A,
    0xB4,1, 0x03,
    0xC0,3, 0x64,0x04,0x84,
    0xC1,1, 0xC5,
    0xC2,2, 0x0D,0x00,
    0xC3,2, 0x8D,0x2A,
    0xC4,2, 0x8D,0xEE,
    0xC5,1, 0x0E,
    0xE0,16, 0x15,0x0B,0x02,0x00,0x08,0x00,0x00,0x00,0x00,0x05,0x11,0x35,0x10,0x12,0x05,0x3F,
    0xE1,16, 0x0E,0x0E,0x03,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x12,0x37,0x10,0x10,0x06,0x3F,
    0x3A,1, 0x05,                            // COLMOD 16-bit
    0x36,1, 0xC8,                            // MADCTL
    0x21,0,                                  // INVON (this panel shows black correctly only with it)
    0x29,0,                                  // display on
    0x00
};

static void run_init(void)
{
    int i = 0, n = sizeof(INIT);
    while (i < n) {
        uint8_t cmd = INIT[i++];
        if (cmd == 0xFF) { vTaskDelay(pdMS_TO_TICKS(INIT[i++]) + 1); continue; }
        if (i >= n) break;
        uint8_t na = INIT[i++];
        lcd_cmd(cmd);
        if (na) { lcd_data(&INIT[i], na); i += na; }
    }
}

static void lcd_bl(bool on)   // LEDC ch4 / GPIO21, active-low
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, on ? 0 : 8191);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static void set_window(int x, int y, int w, int h)
{
    int x0 = x + COL_OFFSET, x1 = x + w - 1 + COL_OFFSET;
    int y0 = y + ROW_OFFSET, y1 = y + h - 1 + ROW_OFFSET;
    uint8_t c[4];
    lcd_cmd(0x2A); c[0]=x0>>8; c[1]=x0&0xFF; c[2]=x1>>8; c[3]=x1&0xFF; lcd_data(c,4);
    lcd_cmd(0x2B); c[0]=y0>>8; c[1]=y0&0xFF; c[2]=y1>>8; c[3]=y1&0xFF; lcd_data(c,4);
    lcd_cmd(0x2C);
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    set_window(x, y, w, h);
    static uint8_t line[LCD_W * 2];
    for (int i = 0; i < w; i++) { line[2*i] = color >> 8; line[2*i+1] = color & 0xFF; }
    gpio_set_level(PIN_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = { .length = 8 * 2 * w, .tx_buffer = line };
        spi_device_polling_transmit(s_spi, &t);
    }
}

// ---- compact 5x7 digit/symbol font (columns, bit0 = top row) ----
// Index: '0'..'9' = 0..9, ':' = 10, '%' = 11, '-' = 12, ' ' = 13.
static const uint8_t FONT[14][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x00,0x00,0x00,0x00}, // space
};
static int glyph(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == '%') return 11;
    if (c == '-') return 12;
    return 13;
}
// Draw a char at (x,y), each font pixel a sxs block; the whole cell is cleared to
// bg first, so redrawing overwrites cleanly (no full-screen wipe / flicker).
static void draw_char(int x, int y, int s, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *g = FONT[glyph(c)];
    fill_rect(x, y, 5 * s, 7 * s, bg);
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row)) fill_rect(x + col * s, y + row * s, s, s, fg);
}
// Draw a string; returns the x after it. Advances 6*s per char (5 + 1 gap).
static int draw_text(int x, int y, int s, const char *str, uint16_t fg, uint16_t bg)
{
    for (; *str; str++) { draw_char(x, y, s, *str, fg, bg); x += 6 * s; }
    return x;
}
static int text_w(const char *str, int s) { return (int)strlen(str) * 6 * s; }

void hw_display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<PIN_DC)|(1ULL<<PIN_RST),
        .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 1600,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG,"spi bus"); return; }
    spi_device_interface_config_t dev = {
        .mode = 0, .clock_speed_hz = 30*1000*1000, .spics_io_num = PIN_CS, .queue_size = 7,
    };
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) { ESP_LOGE(TAG,"spi dev"); return; }
    // reset (use us delays — the 100 Hz tick makes pdMS_TO_TICKS(10) unreliable)
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1); esp_rom_delay_us(10000);
    gpio_set_level(PIN_RST, 0); esp_rom_delay_us(10000);
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120) + 1);
    run_init();
    fill_rect(0, 0, LCD_W, LCD_H, BLACK);
    lcd_bl(true);   // assert the backlight on (never let an LED write turn it off)
    ESP_LOGI(TAG, "display up (ST7735S 80x160 SPI2, backlight LEDC ch4/GPIO21)");
}

// ---- clean status UI ----
// Battery % (large) + bar, brushing MM:SS timer (green while brushing), gear bars,
// and Wi-Fi / charge dots. Elements self-clear their cell, so only what changed is
// repainted — no full-screen flashing.
static void draw_battery(int pct)
{
    char s[6]; snprintf(s, sizeof s, "%d%%", pct);
    int w = text_w(s, 3) - 3;               // minus the trailing gap
    draw_text((LCD_W - w) / 2, 8, 3, s, pct <= 20 ? RED : WHITE, BLACK);
    // bar
    fill_rect(8, 34, 64, 8, GREY);
    int fill = (60 * (pct < 0 ? 0 : pct)) / 100;
    fill_rect(10, 36, fill, 4, pct <= 20 ? RED : GREEN);
    fill_rect(10 + fill, 36, 60 - fill, 4, GREY);
}
static void draw_timer(bool brushing, int secs)
{
    char s[12];
    int mins = secs / 60; if (mins > 99) mins = 99;
    if (brushing) snprintf(s, sizeof s, "%d:%02d", mins, secs % 60);
    else          strcpy(s, "-:--");
    int w = text_w(s, 3) - 3;
    // clear the timer band, then centre the text
    fill_rect(0, 58, LCD_W, 7 * 3, BLACK);
    draw_text((LCD_W - w) / 2, 58, 3, s, brushing ? GREEN : DIM, BLACK);
}
static void draw_gear(int gear, bool brushing)
{
    for (int i = 0; i < 5; i++)
        fill_rect(8 + i * 13, 96, 10, 18, i < gear ? (brushing ? BLUE : DIM) : GREY);
}
static void draw_dots(bool wifi, bool charging)
{
    fill_rect(14, 134, 14, 14, wifi ? GREEN : GREY);       // left: Wi-Fi link
    fill_rect(52, 134, 14, 14, charging ? GREEN : GREY);   // right: charging
}

static void lcd_task(void *arg)
{
    bool first = true;
    int l_pct = -999, l_gear = -1, elapsed = 0;
    bool l_br = false, l_wifi = false, l_charge = false, l_timer_br = false;
    int l_shown_sec = -1;
    while (1) {
        brush_state_t b; metrics_get_brush_state(&b);
        bool wifi = wifi_mgr_is_connected();
        bool charging = b.charging;
        bool brushing = b.brushing;
        int pct = b.battery_pct < 0 ? 0 : b.battery_pct;
        int gear = b.mode < 0 ? 0 : b.mode;

        if (brushing && !l_br) elapsed = 0;       // new session -> reset timer
        else if (brushing) elapsed++;

        if (first) { fill_rect(0, 0, LCD_W, LCD_H, BLACK); }
        if (first || pct != l_pct) draw_battery(pct);
        if (first || brushing != l_timer_br || (brushing && elapsed != l_shown_sec)) {
            draw_timer(brushing, elapsed); l_timer_br = brushing; l_shown_sec = elapsed;
        }
        if (first || gear != l_gear || brushing != l_br) draw_gear(gear, brushing);
        if (first || wifi != l_wifi || charging != l_charge) draw_dots(wifi, charging);

        if (first || pct != l_pct || gear != l_gear || brushing != l_br) {
            ESP_LOGI(TAG, "lcd: battery=%d%% gear=%d brushing=%d charging=%d wifi=%d", pct, gear, brushing, charging, wifi);
        }
        first = false; l_pct = pct; l_gear = gear; l_br = brushing; l_wifi = wifi; l_charge = charging;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void hw_display_start(void) { xTaskCreate(lcd_task, "lcd", 4096, NULL, 4, NULL); }
