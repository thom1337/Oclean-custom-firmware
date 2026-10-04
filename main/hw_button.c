#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hardware.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"

// The button on GPIO3 (active low), done the way the stock firmware does it
// (input.md section 3): everything happens in the GPIO interrupt plus four one-shot
// timers that are started on every press.
//   release after 80..1499 ms            -> code 0 (short press)
//   still down after 2 / 3 / 5 / 8 s     -> code 1 / 2 / 3 / 4 (a long hold delivers
//                                           each of them in turn)
// A press shorter than 80 ms or released between 1.5 s and 2 s produces nothing.
// Each event is handed to the core with oem_button_push() and OEM_EV_BUTTON; the
// main task then runs the dispatcher. The 8 s hold also raises the UI task's
// factory-reset event, as stock does (0x4201fd54).
static const char *TAG = "button";

#define BTN HW_PIN_BUTTON

static esp_timer_handle_t s_t2s, s_t3s, s_t5s, s_t8s, s_twdt;
static volatile uint8_t  s_released = 1;   // 0x3fc9f302
static volatile uint8_t  s_pressed;        // 0x3fca4d9b
static volatile uint8_t  s_wdt_flag;       // 0x3fca4d9c
static volatile uint32_t s_press_ms;       // 0x3fca4db8

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// 0x4201b680 (the low-battery auto-sleep cancel it also does is core state and
// belongs to oem_button_push)
static void post(uint8_t code)
{
    oem_button_push(code);
    if (g_oem.init_ok >= 5) hal_event_post(OEM_EV_BUTTON);
}

static void stop_hold_timers(void)
{
    esp_timer_stop(s_t2s);
    esp_timer_stop(s_t8s);
    esp_timer_stop(s_t3s);
    esp_timer_stop(s_t5s);
}

// 0x40377dc0
static void on_press(void)
{
    // Stock restarts the system here when the previous press's 100 us timer has not
    // fired yet, i.e. when the esp_timer task is stuck ("sys_abnormal == 1"). It
    // calls esp_restart() from the interrupt; a panic restart is the form of that
    // which is valid in this context.
    if (s_wdt_flag == 1) esp_system_abort("button: esp_timer task stuck (stock sys_abnormal)");
    s_press_ms = now_ms();
    stop_hold_timers();
    esp_timer_start_once(s_t2s, 2000000);
    esp_timer_start_once(s_t8s, 8000000);
    esp_timer_start_once(s_t3s, 3000000);
    esp_timer_start_once(s_t5s, 5000000);
    s_wdt_flag = 1;
    esp_timer_start_once(s_twdt, 100);
    s_pressed = 1;
}

// 0x40377e74
static void on_release(void)
{
    uint32_t held = now_ms() - s_press_ms;
    if (held >= 80 && held <= 1499 && s_pressed == 1) post(0);
    s_pressed = 0;
    stop_hold_timers();
}

// 0x40377c98, pin 3 branch. Edges before the main task is up are ignored.
static void button_isr(void *arg)
{
    (void)arg;
    if (g_oem.init_ok < 5) return;
    if (gpio_get_level(BTN) == 0) {
        if (s_released != 1) return;           // already down
        on_press();
        s_released = 0;
    } else {
        on_release();
        s_released = 1;
    }
}

// Hold timers (0x42019cb4, 0x42019c9c, 0x42019c84, 0x42019ccc): nothing if the
// button was released in the meantime.
static void hold_cb(void *arg)
{
    if (gpio_get_level(BTN) != 0) return;
    uint8_t code = (uint8_t)(uintptr_t)arg;
    post(code);
    if (code == 4) hal_ui_event_post(OEM_UIEV_FACTORY);
}

static void wdt_cb(void *arg) { (void)arg; s_wdt_flag = 0; }   // 0x42019c74

static esp_timer_handle_t make_timer(const char *name, esp_timer_cb_t cb, uintptr_t arg)
{
    esp_timer_handle_t h = NULL;
    const esp_timer_create_args_t a = { .callback = cb, .arg = (void *)arg, .name = name };
    ESP_ERROR_CHECK(esp_timer_create(&a, &h));
    return h;
}

// brush_button_init 0x42019cf8 plus the GPIO3 part of brush_gpio_cfg 0x4200d77c
void oem_button_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    s_t2s  = make_timer("btnperiodic1", hold_cb, 1);
    s_t8s  = make_timer("btnperiodic2", hold_cb, 4);
    s_t3s  = make_timer("btnperiodic3", hold_cb, 2);
    s_t5s  = make_timer("btnperiodic5", hold_cb, 3);
    s_twdt = make_timer("wdtcheck", wdt_cb, 0);

    gpio_hold_dis(BTN);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN, button_isr, NULL));

    // Not stock: a hold that began before this point (the press that woke the brush from
    // deep sleep, or a power-on with the button held) is never seen as a press, so the 8 s
    // factory-reset hold, which also clears the web password, needed a second press once
    // the brush was awake. For such a hold only the 8 s timer is armed, counted from the
    // boot: hold_cb acts only if the button is still down then, and a release seen
    // before that cancels it (on_release). The shorter holds keep stock's behaviour. Not
    // after a software restart: the factory reset ends in one, and a button still held
    // through it must not reset the brush again 8 s later. Not under emulation either:
    // QEMU reads the pin low, which would reset every emulated run at 8 s.
    esp_reset_reason_t why = esp_reset_reason();
    if ((why == ESP_RST_DEEPSLEEP || why == ESP_RST_POWERON) && !hw_emulated() && gpio_get_level(BTN) == 0) {
        uint32_t t = now_ms();
        esp_timer_start_once(s_t8s, (uint64_t)(t < 7000 ? 8000 - t : 1000) * 1000);
        ESP_LOGI(TAG, "button held at start: the 8 s hold counts from the boot");
    }
}
