#include "driver/gpio.h"
#include "esp_log.h"
#include "hardware.h"
#include "hw_bus.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_input.h"
#include "oem_state.h"

// IQS7222D trackpad controller (input.md section 1). The chip talks only inside
// the communication window it announces by pulling RDY (GPIO12) low. Each falling
// edge posts OEM_EV_TOUCH_RDY and the main task then runs one step of the state
// machine below, as stock does. The byte sequences are the stock ones; register
// names in the comments are inferred from the public IQS7222 map.
static const char *TAG = "touch";

#define RDY  HW_PIN_TOUCH_RDY
#define ADDR HW_IQS7222D_ADDR

static uint8_t  s_state;          // 0x3fca4ded
static uint8_t  s_rdy_count;      // 0x3fca4dec, RDY events since force-comms (saturates at 100)
static uint16_t s_status[6];      // 0x3fca5a48, registers 0x10..0x15
static uint8_t  s_no_touch_s;     // 0x3fca5a17
static uint8_t  s_touch_s;        // 0x3fca5a16
static uint8_t  s_tick;           // 10 ms ticks towards the 1 Hz reseed check
static bool     s_setup;          // oem_touch_init() has run
static uint8_t  s_missed;         // status reads the chip did not answer (log only)

// Byte [1] of the setup block of the eight trackpad channels (0, 1, 2, 3, 7, 8, 9,
// 10). The stock .data arrays start with 0x5D; the sleep sequence sends 0x54 and
// leaves 0x55 in RAM, so every init after the first sleep sends 0x55 (input.md 1.5).
static uint8_t  s_chan_b1 = 0x5D;

// ---- init sequence, stock 0x42025f2c(0): 67 writes -----------------------------------
// Generated from the stock image (re/spec/input.md 1.5). W8 = one-byte register,
// W16 = two-byte register (high byte first).
enum { W8, W16 };
typedef struct {
    uint8_t kind, reg_hi, reg_lo, len;
    uint8_t d[28];
} iqs_write_t;

static const iqs_write_t k_init[] = {
    /*  0 */ { W8 , 0x00, 0xDB,  1, { 0x0D } },
    /*  1 */ { W8 , 0x00, 0xD0, 19, { 0x00, 0x00, 0x02, 0x00, 0xFF, 0xFF, 0x05, 0x00, 0x0C, 0x00, 0x05, 0x00, 0x28, 0x00, 0x0A, 0x00, 0x64, 0x00, 0xFF } },
    /*  2 */ { W16, 0x80, 0x00, 28, { 0x7F, 0x05, 0x62, 0x70, 0x7F, 0x05, 0x62, 0x10, 0x7F, 0x05, 0x62, 0x20, 0x7F, 0x05, 0x62, 0x40, 0x7F, 0x0C, 0x62, 0x80, 0x7F, 0x0C, 0x62, 0x20, 0x7F, 0x0C, 0x62, 0x04 } },
    /*  3 */ { W16, 0x87, 0x00,  6, { 0x83, 0x2B, 0x10, 0x30, 0x00, 0x02 } },
    /*  4 */ { W16, 0x90, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /*  5 */ { W16, 0x91, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x14, 0x28 } },
    /*  6 */ { W16, 0x92, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x14, 0x28 } },
    /*  7 */ { W16, 0x93, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x14, 0x28 } },
    /*  8 */ { W16, 0x94, 0x00,  6, { 0x0A, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /*  9 */ { W16, 0x95, 0x00,  6, { 0x0A, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /* 10 */ { W16, 0x96, 0x00,  6, { 0x0A, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /* 11 */ { W16, 0x97, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /* 12 */ { W16, 0x98, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x14, 0x28 } },
    /* 13 */ { W16, 0x99, 0x00,  6, { 0x08, 0x12, 0x0F, 0x00, 0x14, 0x28 } },
    /* 14 */ { W16, 0x9A, 0x00,  6, { 0x83, 0x2B, 0x10, 0x30, 0x00, 0x02 } },
    /* 15 */ { W16, 0x9B, 0x00,  6, { 0x0A, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /* 16 */ { W16, 0x9C, 0x00,  6, { 0x0A, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /* 17 */ { W16, 0x9D, 0x00,  6, { 0x0A, 0x12, 0x0F, 0x00, 0x08, 0x28 } },
    /* 18 */ { W16, 0xA0, 0x00,  8, { 0x33, 0x5D, 0x45, 0x64, 0xE4, 0x2F, 0xEA, 0x69 } },
    /* 19 */ { W16, 0xA1, 0x00,  8, { 0x13, 0x5D, 0x45, 0x60, 0xE1, 0x39, 0xE8, 0x69 } },
    /* 20 */ { W16, 0xA2, 0x00,  8, { 0x13, 0x5D, 0x45, 0x60, 0xE1, 0x39, 0xDE, 0x69 } },
    /* 21 */ { W16, 0xA3, 0x00,  8, { 0x13, 0x5D, 0x45, 0x60, 0xE1, 0x35, 0xEB, 0x71 } },
    /* 22 */ { W16, 0xA4, 0x00,  8, { 0x23, 0x14, 0x3D, 0x80, 0xE1, 0x35, 0xFE, 0x69 } },
    /* 23 */ { W16, 0xA5, 0x00,  8, { 0x23, 0x14, 0x3D, 0x3E, 0xE1, 0x39, 0xE4, 0x61 } },
    /* 24 */ { W16, 0xA6, 0x00,  8, { 0x23, 0x14, 0x3D, 0x3E, 0xE1, 0x39, 0xFB, 0x69 } },
    /* 25 */ { W16, 0xA7, 0x00,  8, { 0x13, 0x5D, 0x45, 0x64, 0xE4, 0x31, 0xEE, 0x71 } },
    /* 26 */ { W16, 0xA8, 0x00,  8, { 0x13, 0x5D, 0x45, 0x60, 0xE1, 0x3B, 0xF5, 0x71 } },
    /* 27 */ { W16, 0xA9, 0x00,  8, { 0x13, 0x5D, 0x45, 0x60, 0xE1, 0x39, 0xDB, 0x69 } },
    /* 28 */ { W16, 0xAA, 0x00,  8, { 0x13, 0x5D, 0x45, 0x60, 0xE1, 0x35, 0xF0, 0x69 } },
    /* 29 */ { W16, 0xAB, 0x00,  8, { 0x23, 0x14, 0x3D, 0x3E, 0xE1, 0x35, 0xFA, 0x61 } },
    /* 30 */ { W16, 0xAC, 0x00,  8, { 0x23, 0x14, 0x3D, 0x3E, 0xE1, 0x3D, 0xFA, 0x69 } },
    /* 31 */ { W16, 0xAD, 0x00,  8, { 0x23, 0x14, 0x3D, 0x3E, 0xE1, 0x35, 0xF0, 0x61 } },
    /* 32 */ { W16, 0xAE, 0x00,  3, { 0x42, 0xD8, 0xD8 } },
    /* 33 */ { W16, 0xB0, 0x00,  2, { 0x23, 0x0A } },
    /* 34 */ { W16, 0xB0, 0x01,  2, { 0x3C, 0x3C } },
    /* 35 */ { W16, 0xB0, 0x02,  2, { 0x3C, 0x3C } },
    /* 36 */ { W16, 0xB0, 0x03,  2, { 0x1A, 0x36 } },
    /* 37 */ { W16, 0xB0, 0x04,  2, { 0xFF, 0x00 } },
    /* 38 */ { W16, 0xB0, 0x05,  2, { 0xFF, 0x00 } },
    /* 39 */ { W16, 0xB0, 0x06,  2, { 0x8F, 0xC7 } },
    /* 40 */ { W16, 0xB0, 0x07,  2, { 0xEE, 0x06 } },
    /* 41 */ { W16, 0xB0, 0x08,  2, { 0x52, 0x04 } },
    /* 42 */ { W16, 0xB0, 0x09,  2, { 0x74, 0x04 } },
    /* 43 */ { W16, 0xB0, 0x0A,  2, { 0x96, 0x04 } },
    /* 44 */ { W16, 0xB0, 0x0B,  2, { 0x40, 0x05 } },
    /* 45 */ { W16, 0xB0, 0x0C,  2, { 0x62, 0x05 } },
    /* 46 */ { W16, 0xB0, 0x0D,  2, { 0x84, 0x05 } },
    /* 47 */ { W16, 0xB0, 0x0E,  2, { 0x00, 0x00 } },
    /* 48 */ { W16, 0xB0, 0x0F,  2, { 0x00, 0x00 } },
    /* 49 */ { W16, 0xB0, 0x10,  2, { 0x00, 0x00 } },
    /* 50 */ { W16, 0xB0, 0x11,  2, { 0x00, 0x00 } },
    /* 51 */ { W16, 0xB0, 0x12,  2, { 0x00, 0x00 } },
    /* 52 */ { W16, 0xB0, 0x13,  2, { 0x00, 0x00 } },
    /* 53 */ { W16, 0xB0, 0x14,  8, { 0x1F, 0x0F, 0x4D, 0x64, 0x00, 0x00, 0x00, 0x00 } },
    /* 54 */ { W16, 0xC0, 0x00,  2, { 0x00, 0x00 } },
    /* 55 */ { W16, 0xC0, 0x01,  2, { 0x00, 0x00 } },
    /* 56 */ { W16, 0xC0, 0x02,  2, { 0x00, 0x00 } },
    /* 57 */ { W16, 0xC0, 0x03,  2, { 0x00, 0x00 } },
    /* 58 */ { W16, 0xC0, 0x04,  2, { 0x00, 0x00 } },
    /* 59 */ { W16, 0xC0, 0x05,  2, { 0x00, 0x00 } },
    /* 60 */ { W16, 0xC0, 0x06,  2, { 0x00, 0x00 } },
    /* 61 */ { W16, 0xC0, 0x07,  2, { 0x00, 0x00 } },
    /* 62 */ { W16, 0xC0, 0x08,  2, { 0x00, 0x00 } },
    /* 63 */ { W8 , 0x00, 0xDB,  1, { 0x0C } },
    /* 64 */ { W8 , 0x00, 0xDC,  1, { 0x00 } },
    /* 65 */ { W8 , 0x00, 0xDD,  2, { 0x1E, 0x00 } },
    /* 66 */ { W8 , 0x00, 0xFF,  1, { 0x00 } },
};
#define INIT_WRITES (sizeof(k_init) / sizeof(k_init[0]))
_Static_assert(INIT_WRITES == 67, "stock sends 67 writes");

static bool trackpad_channel(const iqs_write_t *w)
{
    if (w->kind != W16 || w->reg_lo != 0) return false;
    return (w->reg_hi >= 0xA0 && w->reg_hi <= 0xA3) || (w->reg_hi >= 0xA7 && w->reg_hi <= 0xAA);
}

static void send(const iqs_write_t *w, int chan_b1)
{
    uint8_t d[28];
    for (int i = 0; i < w->len; i++) d[i] = w->d[i];
    if (chan_b1 >= 0 && trackpad_channel(w)) d[1] = (uint8_t)chan_b1;
    if (w->kind == W8) hw_bus_write8(ADDR, w->reg_lo, d, w->len);
    else               hw_bus_write16(ADDR, w->reg_hi, w->reg_lo, d, w->len);
}

static void write1(uint8_t reg, uint8_t v) { hw_bus_write8(ADDR, reg, &v, 1); }

static void window_hold(void)  { write1(0xDB, 0x0D); }   // 0x42025ee4: keep the window open across STOP
static void window_close(void) { write1(0xFF, 0x00); }   // 0x42025f08; outside a window: "force comms" (0x42025ebc)

// 0x42025f2c(0). Runs only while RDY is low (inside a window); returns false when
// it was skipped.
static bool iqs_init(void)
{
    if (gpio_get_level(RDY) != 0) return false;
    for (unsigned i = 0; i < INIT_WRITES; i++) send(&k_init[i], s_chan_b1);
    return true;
}

// 0x42026434: read 8 bytes from register 0. Stock only prints the type; the driver
// uses the IQS7222D sequence whatever the answer is.
static void iqs_probe(void)
{
    uint8_t id[8] = {0};
    bool ack = hw_bus_read(ADDR, 0x00, id, 8);
    if (id[0] == 0x16 && id[1] == 0x04 && id[2] == 0x00) ESP_LOGI(TAG, "IQS7222D found");
    else ESP_LOGW(TAG, "unexpected touch id %02x %02x %02x (ack %d)", id[0], id[1], id[2], ack);
}

// 0x420269b0: registers 0x10..0x15. False when the chip did not acknowledge (it
// answers only inside its window); the words are then all ones.
static bool iqs_read_status(void)
{
    uint8_t b[12] = {0};
    bool ack = hw_bus_read(ADDR, 0x10, b, 12);
    for (int i = 0; i < 6; i++) s_status[i] = (uint16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    return ack;
}

// 0x42026814: put the trackpad channels to sleep.
static void iqs_sleep(void)
{
    static const uint8_t order[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA8, 0xA9, 0xAA, 0xA7 };
    window_hold();
    for (int k = 0; k < 8; k++) {
        for (unsigned i = 0; i < INIT_WRITES; i++) {
            if (k_init[i].kind == W16 && k_init[i].reg_hi == order[k] && k_init[i].reg_lo == 0) {
                send(&k_init[i], 0x54);
                break;
            }
        }
    }
    write1(0xD0, 0x20);
    iqs_read_status();                       // result unused, as in stock
    static const uint8_t zero2[2] = { 0, 0 };
    hw_bus_write8(ADDR, 0xDA, zero2, 2);
    write1(0x13, 0x00);
    write1(0xD0, 0xA0);
    s_chan_b1 = 0x55;
    window_close();
}

// GPIO12 interrupt. Stock 0x40377c98 (pin 12 branch): edges before the main task is
// up are ignored.
static void rdy_isr(void *arg)
{
    (void)arg;
    if (g_oem.init_ok >= 5 && gpio_get_level(RDY) == 0) hal_event_post(OEM_EV_TOUCH_RDY);
}

void oem_touch_init(void)
{
    s_state = 0;
    s_rdy_count = 0;
    s_setup = true;
    if (hw_emulated()) return;
    hw_bus_init();
    // 0x4200da4c: RDY is driven low from boot until the main task enables the
    // interrupt (input.md 1.3; the reason is not known, it is kept as is).
    gpio_hold_dis(RDY);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RDY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_set_level(RDY, 0);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(err));
}

// 0x4200cc58
void oem_touch_irq(bool enable)
{
    if (!s_setup) oem_touch_init();              // not done at boot: at least prepare the pins now
    if (hw_emulated()) return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RDY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    if (enable) gpio_isr_handler_add(RDY, rdy_isr, NULL);
    else        gpio_isr_handler_remove(RDY);
}

// 0x4201b430
void oem_touch_set_state(uint8_t st)
{
    if (!s_setup) oem_touch_init();
    if (st == 0) {
        if (!hw_emulated()) window_close();      // 0x42025ebc: force comms
        s_rdy_count = 0;
    }
    s_state = st;
}

// 0x3fca4ded. Under QEMU there is no chip: report "running" so that a session can
// be started with the button (the short-press handler requires state 5).
uint8_t oem_touch_state(void)
{
    return hw_emulated() ? 5 : s_state;
}

// 0x4201b458: one step per RDY falling edge.
void oem_touch_step(void)
{
    if (hw_emulated()) return;
    if (s_rdy_count < 100) s_rdy_count++;
    switch (s_state) {
    case 0:
        s_state = 0x20;
        break;
    case 0x20:
        iqs_probe();
        s_state = 0x21;
        break;
    case 0x21:
        if (iqs_init()) {
            ESP_LOGI(TAG, "init sent (channel byte %02x)", s_chan_b1);
            s_state = 1;
        }
        break;
    case 1:
        write1(0xD0, 0x05);                      // 0x420267b4: ack reset + re-ATI
        s_state = 5;
        break;
    case 2: case 3: case 10: case 11:
        s_state = 5;
        break;
    case 5:
        if (!iqs_read_status()) {
            // Read outside the window: no data. Stock takes the idle-bus ones for a
            // "reset" status and runs the whole init again; a real reset still shows
            // in the next answered read, so the report is just skipped here.
            if (s_missed < 255 && ++s_missed % 50 == 1) ESP_LOGW(TAG, "status read not acknowledged (%u)", s_missed);
            break;
        }
        if (s_status[0] & 0x000A) {              // reset occurred / ATI error: full init again
            ESP_LOGW(TAG, "status %04x: re-init", s_status[0]);
            s_state = 0x21;
            break;
        }
        // 0x42026780 -> 0x420263fc
        window_hold();
        g_oem.touch_x = s_status[4];
        g_oem.touch_y = s_status[5];
        oem_gesture_sample(s_status[4], s_status[5]);
        window_close();
        break;
    case 6:
        write1(0xD0, 0x08);                      // 0x420267e4: reseed
        s_state = 5;
        break;
    case 7:
        ESP_LOGI(TAG, "channels to sleep");
        iqs_sleep();
        break;
    default:                                     // 4, 8, 9 and unknown: nothing
        break;
    }
}

// The stock main loop runs this check once per second while no session runs or the
// session is paused (0x4201b4f8 -> 0x42025e20): the chip is reseeded every 3 s,
// with or without a finger on it. Call on every 10 ms tick.
void oem_touch_tick_10ms(void)
{
    if (hw_emulated()) return;
    if (++s_tick < 100) return;
    s_tick = 0;
    if (g_oem.session_active && g_oem.stop_delay == 200) return;
    if (s_state != 5) return;
    if (!oem_gesture_touching()) {
        if ((uint8_t)(s_no_touch_s + 1) < 3) s_no_touch_s++;
        else { s_no_touch_s = 0; oem_touch_set_state(6); }
        s_touch_s = 0;
    } else {
        if ((uint8_t)(s_touch_s + 1) < 3) s_touch_s++;
        else { s_touch_s = 0; oem_touch_set_state(6); }
        s_no_touch_s = 0;
    }
}
