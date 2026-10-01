#include "hardware.h"
#include "metrics.h"
#include "mqtt_ha.h"
#include "wifi_mgr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// ST7735S 80x160 RGB565 on SPI2 (recovered from stock FW).
// Pins: MOSI=40 SCLK=39 CS=38 DC=41 RST=42. Backlight pin UNKNOWN (not driven).
#define PIN_MOSI 40
#define PIN_SCLK 39
#define PIN_CS   38
#define PIN_DC   41
#define PIN_RST  42
#define LCD_W 80
#define LCD_H 160
#define COL_OFFSET 24
#define ROW_OFFSET 0

#define RGB(r,g,b) ((uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)))
#define BLACK RGB(0,0,0)
#define WHITE RGB(255,255,255)
#define GREEN RGB(40,200,90)
#define RED   RGB(220,60,50)
#define BLUE  RGB(60,120,230)
#define GREY  RGB(70,80,90)

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
static void lcd_d1(uint8_t b) { lcd_data(&b, 1); }

// huaersheng_0305 init table: {cmd, nargs, args...}; cmd 0xFF,ms = delay.
static const uint8_t INIT[] = {
    0x11,0,                                  // sleep out
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
    0x21,0,                                  // INVON: this panel variant shows colors inverted without it
    0x29,0,                                  // display on
    0x00                                     // sentinel (handled by length)
};

static void run_init(void)
{
    int i = 0, n = sizeof(INIT);
    while (i < n) {
        uint8_t cmd = INIT[i++];
        if (cmd == 0xFF) { vTaskDelay(pdMS_TO_TICKS(INIT[i++])); continue; }
        if (i >= n) break;
        uint8_t na = INIT[i++];
        lcd_cmd(cmd);
        if (na) { lcd_data(&INIT[i], na); i += na; }
        if (cmd == 0x11 || cmd == 0x29) { /* handled by explicit 0xFF delays */ }
    }
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
    set_window(x, y, w, h);
    static uint8_t line[LCD_W * 2];
    for (int i = 0; i < w; i++) { line[2*i] = color >> 8; line[2*i+1] = color & 0xFF; }
    gpio_set_level(PIN_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = { .length = 8 * 2 * w, .tx_buffer = line };
        spi_device_polling_transmit(s_spi, &t);
    }
}

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
    // reset
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    run_init();
    fill_rect(0, 0, LCD_W, LCD_H, BLACK);
    ESP_LOGI(TAG, "display up (ST7735S 80x160 SPI2); backlight pin unknown — screen may need its BL rail");
}

// Simple graphical status: battery bar, brushing block, gear bars, wifi/mqtt dots.
static void draw_status(void)
{
    brush_state_t b; metrics_get_brush_state(&b);
    bool wifi = wifi_mgr_is_connected(), mqtt = mqtt_ha_is_connected();
    int pct = b.battery_pct < 0 ? 0 : b.battery_pct;
    int gear = b.mode < 0 ? 0 : b.mode;

    // Repaint only when something visible changed; the full-screen fill below
    // otherwise wipes and redraws the whole panel every second (visible flashing).
    static bool drawn = false;
    static int l_pct = -1, l_gear = -1;
    static bool l_br = false, l_wifi = false, l_mqtt = false;
    if (drawn && pct == l_pct && gear == l_gear && b.brushing == l_br && wifi == l_wifi && mqtt == l_mqtt)
        return;
    drawn = true; l_pct = pct; l_gear = gear; l_br = b.brushing; l_wifi = wifi; l_mqtt = mqtt;
    ESP_LOGI(TAG, "lcd repaint: battery=%d%% gear=%d brushing=%d wifi=%d mqtt=%d", pct, gear, b.brushing, wifi, mqtt);

    fill_rect(0, 0, LCD_W, LCD_H, BLACK);

    // battery bar (top)
    fill_rect(6, 8, 68, 16, GREY);
    fill_rect(8, 10, (64 * pct) / 100, 12, pct <= 20 ? RED : GREEN);

    // brushing block (middle)
    fill_rect(10, 40, 60, 60, b.brushing ? GREEN : GREY);

    // gear bars (below)
    for (int i = 0; i < 5; i++)
        fill_rect(10 + i*12, 108, 9, 14, i < gear ? BLUE : GREY);

    // status dots: wifi, mqtt
    fill_rect(14, 134, 14, 14, wifi ? GREEN : RED);
    fill_rect(52, 134, 14, 14, mqtt ? GREEN : RED);
}

static void lcd_task(void *arg)
{
    while (1) { draw_status(); vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void hw_display_start(void) { xTaskCreate(lcd_task, "lcd", 4096, NULL, 4, NULL); }
