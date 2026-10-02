#include "hardware.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// ST7735S-class 80x160 RGB565 panel on SPI2, as driven by the stock firmware:
// MOSI=40 SCLK=39 CS=38 DC=41 RST=42, 30 MHz, mode 0, column offset +24.
// The backlight (LEDC channel 4 on GPIO21) belongs to the LED module (oem_led.c).
// Stock picks one of four init tables from the panel id stored in NVS
// ("storage"/"screen_config" bytes 0..1); we do the same.
#define PIN_MOSI HW_PIN_LCD_MOSI
#define PIN_SCLK HW_PIN_LCD_SCLK
#define PIN_CS   HW_PIN_LCD_CS
#define PIN_DC   HW_PIN_LCD_DC
#define PIN_RST  HW_PIN_LCD_RST
#define LCD_W 80
#define LCD_H 160
#define COL_OFFSET 24

static const char *TAG = "hw_lcd";
static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static bool s_awake;         // panel out of sleep (init table sent)
static int  s_panel = -1;

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

// Init tables, byte for byte what stock sends. Format: cmd, n, n data bytes; if n
// has DLY set, one more byte follows: a delay in ms. (No 0xFF marker: the 3022
// table sends real 0xFF commands.)
#define DLY 0x80
static const uint8_t T_0305[] = {       // stock 0x42028ca4 (also its default)
    0x11,0|DLY,120,
    0xB1,3,0x05,0x3A,0x3A, 0xB2,3,0x05,0x3A,0x3A, 0xB3,6,0x05,0x3A,0x3A,0x05,0x3A,0x3A, 0xB4,1,0x03,
    0xC0,3,0x64,0x04,0x84, 0xC1,1,0xC5, 0xC2,2,0x0D,0x00, 0xC3,2,0x8D,0x2A, 0xC4,2,0x8D,0xEE, 0xC5,1,0x0E,
    0xE0,16,0x15,0x0B,0x02,0x00,0x08,0x00,0x00,0x00,0x00,0x05,0x11,0x35,0x10,0x12,0x05,0x3F,
    0xE1,16,0x0E,0x0E,0x03,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x12,0x37,0x10,0x10,0x06,0x3F,
    0x3A,1,0x05, 0x36,1,0xC8, 0x29,0,
};
static const uint8_t T_HUA[] = {        // stock 0x42028924
    0x11,0,
    0xB1,3,0x05,0x3A,0x3A, 0xB2,3,0x05,0x3A,0x3A, 0xB3,6,0x05,0x3A,0x3A,0x05,0x3A,0x3A, 0xB4,1,0x03,
    0xC0,3,0x60,0x00,0x04, 0xC1,1,0xC5, 0xC2,2,0x0D,0x00, 0xC3,2,0x8D,0x2A, 0xC4,2,0x8D,0xEE, 0xC5,1,0x04,
    0x36,1,0xC8,
    0xE0,16,0x10,0x0E,0x02,0x03,0x0E,0x07,0x02,0x07,0x0A,0x12,0x27,0x37,0x00,0x0D,0x0E,0x10,
    0xE1,16,0x10,0x0E,0x03,0x03,0x0F,0x06,0x02,0x08,0x0A,0x13,0x26,0x36,0x00,0x0D,0x0E,0x10,
    0x3A,1,0x05, 0x29,0,
};
static const uint8_t T_HAN2[] = {       // stock 0x42028ab0
    0x11,0,
    0xB1,3,0x05,0x3C,0x3C, 0xB2,3,0x05,0x3C,0x3C, 0xB3,6,0x05,0x3C,0x3C,0x05,0x3C,0x3C, 0xB4,2,0x03,0x02,
    0xC0,3,0xA4,0x04,0x84, 0xC1,1,0x05, 0xC2,2,0x0D,0x00, 0xC3,2,0x8D,0x6A, 0xC4,2,0x8D,0xEE, 0xC5,1,0x21,
    0xE0,16,0x0D,0x0C,0x0C,0x0E,0x0E,0x00,0x00,0x00,0x00,0x09,0x23,0x31,0x00,0x0C,0x03,0x1A,
    0xE1,16,0x0A,0x05,0x06,0x07,0x08,0x01,0x00,0x00,0x00,0x05,0x20,0x2E,0x00,0x0A,0x01,0x1A,
    0xFC,1,0x80, 0xF0,1,0x11, 0xD6,1,0xCB,
    0x3A,1,0x05, 0x36,1,0xC8, 0x21,0, 0x29,0,
    0x2A,4,0x00,0x1A,0x00,0x69, 0x2B,4,0x00,0x01,0x00,0xA0, 0x2C,0,
};
static const uint8_t T_3022[] = {       // stock 0x42028e40 (a different controller)
    0xFF,1,0xA5, 0x3A,1,0x65, 0x51,1,0x14, 0x53,1,0x11, 0x62,1,0x10, 0x86,1,0x00, 0x87,1,0x1A,
    0x88,1,0x0E, 0x89,1,0x18, 0x61,1,0x10, 0x93,1,0x12, 0x94,1,0x10, 0x95,1,0x10, 0x96,1,0x0E,
    0xB2,1,0x0F, 0xB4,1,0x60, 0x91,1,0x10, 0xC1,1,0xF1, 0xC5,1,0xF8, 0xB5,1,0x00, 0xC3,1,0x11, 0x83,1,0x10,
    0x2A,4,0x00,0x84,0x00,0x84, 0x2B,4,0x00,0x00,0x00,0x1F,
    0x2C,64,
      0,0,0,0,0,0,0,0,0,0,0,0x11,0,0x0B,0,0x18,0,0x05,0,0x0D,0,0x0E,0,0x0A,0,0x1E,0,0x18,0,0x1A,0,0x1E,
      0,0,0,0,0,0,0,0,0,0,0,0x0E,0,0x06,0,0x13,0,0x01,0,0x06,0,0x09,0,0x00,0,0x15,0,0x10,0,0x0E,0,0x1A,
    0x2A,4,0x00,0x00,0x00,0x7F, 0x2B,4,0x00,0x00,0x00,0x9F, 0x83,1,0x00, 0xFF,1,0x00,
    0x11,0|DLY,200, 0x21,0, 0x36,1,0x00, 0x29,0,
};
// What rendered correctly on the first unit before the panel id was read: the 0305
// table plus INVON. Used only when NVS has no panel id.
static const uint8_t T_INVON[] = { 0x21,0 };

enum { P_0305, P_HAN2, P_HUA, P_3022, P_0305_INV, P_COUNT };
static const char *const PNAME[P_COUNT] = {
    "huaersheng_0305", "han2", "huaersheng", "huaersheng_3022", "0305+INVON (no panel id in NVS)" };

static void run_table(const uint8_t *t, size_t n)
{
    for (size_t i = 0; i < n; ) {
        uint8_t c = t[i++], nn = t[i++], na = nn & 0x7F;
        lcd_cmd(c);
        if (na) lcd_data(&t[i], na);
        i += na;
        if (nn & DLY) vTaskDelay(pdMS_TO_TICKS(t[i++]) + 1);
    }
}

// Panel variant, chosen like stock (0x42029120): bytes 0..1 of the screen_config
// blob. NVS "oclean"/"lcd_panel" (1..5) overrides it.
static int panel_variant(void)
{
    uint8_t buf[128], ovr = 0; size_t len = sizeof buf;
    nvs_handle_t h; esp_err_t e = ESP_ERR_NVS_NOT_FOUND;
    if (nvs_open("oclean", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "lcd_panel", &ovr); nvs_close(h); }
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) { e = nvs_get_blob(h, "screen_config", buf, &len); nvs_close(h); }
    bool have = e == ESP_OK && len >= 2;
    uint8_t a = have ? buf[0] : 0, b = have ? buf[1] : 0;
    int v = !have ? P_0305_INV
          : (a == 3 && b == 4) ? P_HAN2 : (a == 4 && b == 5) ? P_HUA : (a == 5 && b == 6) ? P_3022 : P_0305;
    if (ovr >= 1 && ovr <= P_COUNT) v = ovr - 1;
    ESP_LOGI(TAG, "panel id: screen_config %s len=%u id=(%u,%u) override=%u -> %s",
             esp_err_to_name(e), have ? (unsigned)len : 0, a, b, ovr, PNAME[v]);
    return v;
}

static void set_window(int x, int y, int w, int h)
{
    int x0 = x + COL_OFFSET, x1 = x + w - 1 + COL_OFFSET, y1 = y + h - 1;
    uint8_t c[4];
    lcd_cmd(0x2A); c[0] = x0 >> 8; c[1] = x0 & 0xFF; c[2] = x1 >> 8; c[3] = x1 & 0xFF; lcd_data(c, 4);
    lcd_cmd(0x2B); c[0] = y >> 8;  c[1] = y & 0xFF;  c[2] = y1 >> 8; c[3] = y1 & 0xFF; lcd_data(c, 4);
    lcd_cmd(0x2C);
}

static void fill_black(void)
{
    static const uint8_t zero[LCD_W * 2];
    set_window(0, 0, LCD_W, LCD_H);
    for (int r = 0; r < LCD_H; r++) lcd_data(zero, sizeof zero);
}

// Reset the panel and send its init table (stock 0x42029120): RST 1/0/1 with 10 ms
// steps, 120 ms, table, black fill. Busy-wait the short steps: at a 100 Hz tick a
// 10 ms vTaskDelay can be almost nothing.
static void panel_init(void)
{
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1); esp_rom_delay_us(10000);
    gpio_set_level(PIN_RST, 0); esp_rom_delay_us(10000);
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120) + 1);
    switch (s_panel) {
    case P_HAN2: run_table(T_HAN2, sizeof T_HAN2); break;
    case P_HUA:  run_table(T_HUA, sizeof T_HUA);   break;
    case P_3022: run_table(T_3022, sizeof T_3022); break;
    case P_0305_INV:
        run_table(T_0305, sizeof T_0305 - 2); run_table(T_INVON, sizeof T_INVON);
        run_table(T_0305 + sizeof T_0305 - 2, 2); break;
    default:     run_table(T_0305, sizeof T_0305);
    }
    fill_black();
    s_awake = true;
}

// Sets up the SPI bus and picks the panel table; the panel itself is initialised by
// hw_display_wake() (the oem core runs the LCD init on every wake, as stock does).
void hw_display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (hw_emulated()) { ESP_LOGW(TAG, "emulated: no panel"); return; }
    // Stock holds these pins through deep sleep and releases them at boot.
    const int held[] = { PIN_CS, PIN_SCLK, PIN_MOSI, PIN_DC, PIN_RST };
    for (unsigned i = 0; i < sizeof held / sizeof held[0]; i++) gpio_hold_dis(held[i]);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_W * LCD_H * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "spi bus"); return; }
    spi_device_interface_config_t dev = {
        .mode = 0, .clock_speed_hz = 30 * 1000 * 1000, .spics_io_num = PIN_CS, .queue_size = 7,
    };
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) { ESP_LOGE(TAG, "spi dev"); s_spi = NULL; return; }
    s_panel = panel_variant();
    ESP_LOGI(TAG, "display bus up (80x160 SPI2, %s)", PNAME[s_panel]);
}

void hw_display_blit(const uint8_t *fb)
{
    if (!s_spi || !fb) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_awake) {
        set_window(0, 0, LCD_W, LCD_H);
        gpio_set_level(PIN_DC, 1);
        spi_transaction_t t = { .length = 8 * LCD_W * LCD_H * 2, .tx_buffer = fb };
        spi_device_polling_transmit(s_spi, &t);
    }
    xSemaphoreGive(s_lock);
}

// Panel to sleep (SLPIN), as stock does at screen-off (it never sends DISPOFF).
void hw_display_sleep(void)
{
    if (!s_spi) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lcd_cmd(0x10); s_awake = false;
    xSemaphoreGive(s_lock);
}

// Screen on: full reset + init table + black frame (stock runs this on every wake).
void hw_display_wake(void)
{
    if (!s_spi) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    panel_init();
    xSemaphoreGive(s_lock);
}

bool hw_display_awake(void) { return s_awake; }
