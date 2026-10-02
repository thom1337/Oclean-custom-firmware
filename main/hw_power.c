#include "hw_power.h"
#include "hardware.h"
#include "boot_guard.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

// Power-management mechanisms of the stock firmware (re/spec/power.md §3, §4.1, §6.1):
// the boot-time wake dispatch with its "go straight back to sleep" checks, the pin
// parking for the screen-off stage and for deep sleep, the two deep-sleep variants,
// the PM locks and the motion interrupt. WHEN to sleep is decided by the app module.
//
// The brush has no serial port and no reset button, so every path here leans towards
// staying awake: the button (GPIO3) wake is armed before anything else is touched and
// a deep sleep is refused when that fails, when the charger is present (it would wake
// at once: EXT1 is "any pin low" and GPIO9 is low on the charger) or under QEMU.

static const char *TAG = "hw_power";

#define BATT_EMPTY_MV        3299   // stock 0xce3: at or below this, off the charger, go back to sleep
#define GYRO_WAKE_LIMIT      4      // gyro_wakeup_to_up_limit 0x4201b6f0: over the limit when count > 4
#define GYRO_WAKE_CAP        20     // 0x4201b714 only counts while below 20
#define ANYMOTION_WINDOW_MS  5000   // stock one-shot timer "anymotion_timeout", 5 000 000 us

static void (*s_pre_sleep_hook)(void);

void hw_power_set_pre_sleep_hook(void (*fn)(void)) { s_pre_sleep_hook = fn; }

static void wait_ms(int ms)
{
    // The tick is 10 ms here (1 ms in stock): short waits are busy-waits.
    if (ms < 10) esp_rom_delay_us(ms * 1000);
    else vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

static void pin_cfg(int pin, gpio_mode_t mode, bool pull_up, bool pull_down, gpio_int_type_t intr)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = mode,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = intr,
    };
    gpio_config(&c);
}

static void pin_out(int pin, int level)
{
    pin_cfg(pin, GPIO_MODE_OUTPUT, false, false, GPIO_INTR_DISABLE);
    gpio_set_level(pin, level);
}

// ---- RTC-retained variables (stock segment 0x50001000, spec §3.3) -------------------
// The initial values are the ones in the stock image; like there, the bootloader
// reloads them on every reset that is not a deep-sleep wake.
#define RTC_DEFAULTS { .magic = OEM_RTC_MAGIC, .ota_oneshot = 1, .strength = 3, .hist_day = 0xff }
static RTC_DATA_ATTR oem_rtc_t s_rtc = RTC_DEFAULTS;

oem_rtc_t *hw_power_rtc(void)
{
    if (s_rtc.magic != OEM_RTC_MAGIC) s_rtc = (oem_rtc_t)RTC_DEFAULTS;   // corrupted: start over
    return &s_rtc;
}

// ---- motion-wake counter: RTC memory, mirrored in NVS -------------------------------
// Stock keeps gyro_wakeup_count in RTC memory and in the NVS blob "wakeupcount"
// (namespace "storage", 10 bytes, byte 0 = count; 0x42023d34 / 0x42023d68).
static bool count_nvs_read(uint8_t *out)
{
    nvs_handle_t h;
    uint8_t b[10];
    size_t n = sizeof b;
    if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_blob(h, "wakeupcount", b, &n);
    nvs_close(h);
    if (e != ESP_OK || n < 1) return false;
    *out = b[0];
    return true;
}

static void count_nvs_write(uint8_t v)
{
    uint8_t cur;
    if (count_nvs_read(&cur) && cur == v) return;      // spare the flash
    nvs_handle_t h;
    uint8_t b[10] = { v };
    if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "wakeupcount", b, sizeof b);
    nvs_commit(h);
    nvs_close(h);
}

static void count_set(uint8_t v)
{
    hw_power_rtc()->gyro_wakeup_count = v;
    count_nvs_write(v);
}

// ---- any-motion window and the GPIO8 interrupt ---------------------------------------
// Stock sets allow_anymotion_check_flag (0x3fca2885) and restarts a 5 s one-shot
// timer that clears it (0x40377c64), also from the ISR. Here the arming time is
// recorded instead, so the ISR does not have to call esp_timer.
static volatile TickType_t s_anymotion_tick;
static volatile bool       s_anymotion_armed;

bool hal_anymotion_allowed(void)
{
    return s_anymotion_armed &&
           (TickType_t)(xTaskGetTickCount() - s_anymotion_tick) < pdMS_TO_TICKS(ANYMOTION_WINDOW_MS);
}

// GPIO8 branch of the stock GPIO ISR (0x40377c98).
static void motion_isr(void *arg)
{
    (void)arg;
    if (g_oem.init_ok < 5) return;
    s_anymotion_tick = xTaskGetTickCountFromISR();
    s_anymotion_armed = true;
    if (g_oem.wake_gate < 4) return;
    if (g_oem.motion_gate != 0) return;
    hal_event_post(OEM_EV_MOTION);
}

static void isr_service(void)
{
    // Several drivers share the per-pin ISR service; whoever comes first installs it.
    static bool done;
    if (done) return;
    done = true;
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "isr service: %d", (int)e);
}

// 0x4200d75c. Stock configures GPIO8 for any-edge interrupts once at boot and only
// adds / removes the handler; the edge type is set here so the pin carries no enabled
// interrupt without a handler.
void oem_motion_irq(bool enable)
{
    if (!enable) {
        gpio_isr_handler_remove(HW_PIN_MOTION_INT);
        return;
    }
    isr_service();
    gpio_set_intr_type(HW_PIN_MOTION_INT, GPIO_INTR_ANYEDGE);
    esp_err_t e = gpio_isr_handler_add(HW_PIN_MOTION_INT, motion_isr, NULL);
    if (e != ESP_OK) ESP_LOGE(TAG, "motion isr: %d", (int)e);
}

// ---- PM locks (spec §6.1) -----------------------------------------------------------
// Stock: esp_pm_configure(160 / 40 MHz, light sleep) with a NO_LIGHT_SLEEP lock held
// forever, so the only effect is frequency scaling: the APB lock is dropped in the
// screen-off stage, the CPU lock is held while brushing. Without CONFIG_PM_ENABLE the
// locks do not exist and these functions only keep the stock bookkeeping.
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_l_apb, s_l_ls, s_l_cpu;
#endif
static bool   s_pm_ready;
static int8_t s_stay_alive;         // stock 0x3fca41b4: 1 = APB lock held
static int    s_cpu_locks;

static void pm_init(void)
{
    if (s_pm_ready) return;
    s_pm_ready = true;
    s_stay_alive = 1;
#if CONFIG_PM_ENABLE
    // brush_pm_init 0x42014000. The locks are taken before esp_pm_configure (stock:
    // after), so there is no moment in which automatic light sleep could start.
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "l_apb", &s_l_apb) != ESP_OK) s_l_apb = NULL;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "l_ls", &s_l_ls) != ESP_OK) s_l_ls = NULL;
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "l_cpu", &s_l_cpu) != ESP_OK) s_l_cpu = NULL;
    if (!s_l_apb || !s_l_ls || !s_l_cpu) ESP_LOGE(TAG, "esp pm lock create failed");
    if (s_l_apb) esp_pm_lock_acquire(s_l_apb);
    if (s_l_ls) esp_pm_lock_acquire(s_l_ls);            // never released
    // "enter_auto_light_sleep" 0x42013fd0 (constants at 0x3c118b2c)
    esp_pm_config_t cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = s_l_ls != NULL,           // never without the lock that prevents it
#endif
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) ESP_LOGE(TAG, "esp_pm_configure: %d", (int)e);
#endif
}

// GPIO20 (LED4) is the USB D+ pad; stock switches its pull-up off before sleeping
// (USB_SERIAL_JTAG_CONF0 bit 9; 0x42014134, 0x4201c5e1).
static void usb_dp_pullup_off(void)
{
    if (hw_emulated()) return;
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
}

// brush_stay_alive 0x420140f4
void oem_power_stay_alive(void)
{
    pm_init();
    if (s_stay_alive < 1) {
        s_stay_alive = 1;
#if CONFIG_PM_ENABLE
        if (s_l_apb) esp_pm_lock_acquire(s_l_apb);
#endif
    }
    g_oem.asleep = 0;
}

// brush_resume_sleep 0x42014134
void oem_power_resume_sleep(void)
{
    pm_init();
    if (s_stay_alive > 0) {
        ESP_LOGI(TAG, "brush_resume_sleep");
        usb_dp_pullup_off();
#if CONFIG_PM_ENABLE
        if (s_l_apb) esp_pm_lock_release(s_l_apb);
#endif
        s_stay_alive = 0;
    }
}

// enable_light_sleep_cpulock 0x42014184 / close_light_sleep_cpulock 0x420141a8.
// Stock aborts on an unbalanced release (ESP_ERROR_CHECK); here it is ignored.
void oem_power_cpu_lock(bool take)
{
    pm_init();
    if (take) {
        s_cpu_locks++;
    } else {
        if (s_cpu_locks == 0) { ESP_LOGW(TAG, "cpu lock released but not held"); return; }
        s_cpu_locks--;
    }
#if CONFIG_PM_ENABLE
    if (!s_l_cpu) return;
    if (take) esp_pm_lock_acquire(s_l_cpu);
    else esp_pm_lock_release(s_l_cpu);
#endif
}

// ---- pin parking (spec §3.2; step numbers are the rows of that table) --------------
// gpio_prep_screen_off 0x4200dbbc (deep = false) and gpio_prep_deep_sleep 0x4200dda4
// (deep = true) differ only in steps 6, 11, 14 and 18.
static void gpio_park(bool deep)
{
    // 1-5 and 17: GPIO17..20 plain outputs at "off" (1, 1, 0, 1) and the backlight
    // GPIO21 open-drain high, all held. The LED module does it (and stops writing
    // LEDC duties); stock parks GPIO21 last, which makes no difference: the backlight
    // was switched off 200 ms earlier.
    oem_led_park();
    // 6 (deep): stock removes its I2C0 driver (SDA36 / SCL35). The port has no such
    // driver and never touches those pins.
    // 7
    pin_cfg(HW_PIN_AUX_IN, GPIO_MODE_INPUT, false, false, GPIO_INTR_NEGEDGE);
    // 8
    pin_out(HW_PIN_GPIO45, 0);
    // 9: touch RDY line off (mode "disable"; stock leaves the interrupt type at falling edge)
    pin_cfg(HW_PIN_TOUCH_RDY, GPIO_MODE_DISABLE, false, false, GPIO_INTR_NEGEDGE);
    gpio_set_level(HW_PIN_TOUCH_RDY, 0);
    gpio_hold_en(HW_PIN_TOUCH_RDY);
    // 10: GPIO26 input (charging allowed), held
    oem_charge_allow(true);
    // 11 (deep): IMU SPI pins as held outputs at 1 (0x4200cd54). The SPI driver stays
    // installed; gpio_config routes the pads back to the GPIO output register.
    if (deep) {
        static const uint8_t spi[] = { HW_PIN_IMU_CS, HW_PIN_IMU_SCLK, HW_PIN_IMU_MOSI, HW_PIN_IMU_MISO };
        for (unsigned i = 0; i < sizeof spi; i++) gpio_hold_dis(spi[i]);
        for (unsigned i = 0; i < sizeof spi; i++) {
            pin_cfg(spi[i], GPIO_MODE_OUTPUT, true, false, GPIO_INTR_DISABLE);
            gpio_set_level(spi[i], 1);
            gpio_hold_en(spi[i]);
        }
    }
    // 12: motor amplifier off (not held here)
    pin_out(HW_PIN_MOTOR_AMP, 0);
    // 13
    pin_out(HW_PIN_LCD_PWR, 1);
    gpio_hold_en(HW_PIN_LCD_PWR);
    // 14: LCD SPI pins, in the stock order CS, MOSI, SCLK
    static const uint8_t lcd[] = { HW_PIN_LCD_CS, HW_PIN_LCD_MOSI, HW_PIN_LCD_SCLK };
    for (unsigned i = 0; i < sizeof lcd; i++) {
        if (!deep) {
            gpio_sleep_set_direction(lcd[i], GPIO_MODE_DISABLE);        // 0x4200cfb8
        } else {
            pin_cfg(lcd[i], GPIO_MODE_INPUT, false, false, GPIO_INTR_DISABLE);   // 0x4200cfd4
            gpio_set_level(lcd[i], 1);
            gpio_hold_en(lcd[i]);
        }
    }
    // 15: LCD reset low
    pin_out(HW_PIN_LCD_RST, 0);
    wait_ms(2);
    gpio_hold_en(HW_PIN_LCD_RST);
    // 16
    pin_out(HW_PIN_LCD_DC, 0);
    gpio_hold_en(HW_PIN_LCD_DC);
    // 18 (screen-off stage only)
    if (!deep) gpio_force_unhold_all();
}

void oem_power_prep_screen_off(void)
{
    ESP_LOGI(TAG, "pins: screen-off stage");
    gpio_park(false);
}

// gpio_restore_after_screen_off 0x4200df88. The display (lcd init), the touch IC and
// the pressure sensor are brought back by their owners after this (see NOTES_power).
void oem_power_restore_after_screen_off(void)
{
    static const uint8_t held[] = {
        HW_PIN_BUS_SCL, HW_PIN_BUS_SDA, HW_PIN_BACKLIGHT, HW_PIN_LCD_RST, HW_PIN_LED1, HW_PIN_LED2,
        HW_PIN_LED3, HW_PIN_LED4, HW_PIN_LCD_PWR, HW_PIN_LCD_DC, HW_PIN_TOUCH_RDY,
    };
    ESP_LOGI(TAG, "pins: awake");
    for (unsigned i = 0; i < sizeof held; i++) gpio_hold_dis(held[i]);
    // GPIO17..20 back to "off" as plain outputs, then led_init (0x4200d2a8): LEDC
    // timer and the five channels again, everything dark.
    oem_led_init();
    pin_cfg(HW_PIN_AUX_IN, GPIO_MODE_INPUT, false, false, GPIO_INTR_NEGEDGE);
    pin_out(HW_PIN_GPIO45, 0);
    pin_cfg(HW_PIN_TOUCH_RDY, GPIO_MODE_INPUT, true, false, GPIO_INTR_NEGEDGE);
    oem_charge_allow(true);
    pin_out(HW_PIN_MOTOR_AMP, 0);
    pin_out(HW_PIN_LCD_PWR, 0);
    pin_out(HW_PIN_LCD_DC, 1);
}

// ---- deep sleep (spec §3.1) ---------------------------------------------------------
// Wake sources of "deep sleep1" (0x42014220: EXT1 only) and "deep sleep2" (0x420141cc:
// EXT1 + EXT0). EXT1 mode 0 on the ESP32-S3 wakes when ANY selected pin is low (this
// IDF still names the value ALL_LOW). False = the button wake could not be armed.
static bool wake_sources(bool motion)
{
    esp_err_t e = esp_sleep_enable_ext1_wakeup((1ULL << HW_PIN_BUTTON) | (1ULL << HW_PIN_CHARGER),
                                               ESP_EXT1_WAKEUP_ALL_LOW);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "EXT1 (button / charger) wake not armed (%d): not sleeping", (int)e);
        return false;
    }
    if (motion) {
        e = esp_sleep_enable_ext0_wakeup(HW_PIN_MOTION_INT, 1);
        if (e != ESP_OK) ESP_LOGE(TAG, "EXT0 (motion) wake not armed (%d); button wake only", (int)e);
    }
    return true;
}

// The common end of 0x42014240 / 0x420141f8. Does not return.
static void sleep_start(bool motion)
{
    // Stock disables the timer wake although it never arms it; IDF answers with an
    // error line in the log, which is expected.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    ESP_LOGW(TAG, "deep sleep%d (wake: button, charger%s)", motion ? 2 : 1, motion ? ", motion" : "");
    boot_guard_clean_exit();
    esp_deep_sleep_start();
}

// gpio_prep_deep_sleep 0x4200dda4 + "deep sleep2" 0x420141f8 (motion wake) or
// "deep sleep1" 0x42014240. Returns only when the sleep was refused.
void oem_power_deep_sleep(bool motion_wake)
{
    if (hw_emulated()) {
        ESP_LOGW(TAG, "emulated: deep sleep%d skipped", motion_wake ? 2 : 1);
        return;
    }
    if (oem_charger_present()) {            // stock never gets here on the charger (0x4201c5db)
        ESP_LOGW(TAG, "charger present: deep sleep refused");
        return;
    }
    if (!wake_sources(motion_wake)) return;
    // From here on the sleep is certain.
    if (s_pre_sleep_hook) s_pre_sleep_hook();
    usb_dp_pullup_off();                                   // first step of brush_pm_control 0x4201c5d0
    count_nvs_write(hw_power_rtc()->gyro_wakeup_count);    // keep the NVS copy in step
    gpio_park(true);
    sleep_start(motion_wake);
}

// ---- boot (spec §4.1) ---------------------------------------------------------------
// "qmi_amd_mode(); deep_sleep1();" of brush_gpio_cfg: straight back to sleep, EXT1
// only, without the pin parking (stock does not park here either). Returns only when
// the button wake could not be armed; the boot then continues.
static void boot_sleep(void)
{
    oem_imu_amd();
    if (!wake_sources(false)) return;
    if (s_pre_sleep_hook) s_pre_sleep_hook();
    sleep_start(false);
}

// The checks of brush_gpio_cfg 0x4200da21..0x4200dbb6. Stock codes them as loops, but
// every path leaves on its first pass.
static void boot_sleep_checks(bool by_motion)
{
    oem_rtc_t *rtc = hw_power_rtc();

    if (oem_charger_present()) {            // on the charger the brush always boots
        ESP_LOGI(TAG, "charger present, battery %d mV", oem_batt_mv_now());
        return;
    }
    int mv = oem_batt_mv_now();
    if (mv < OEM_BATT_MV_MIN_VALID) {
        // Stock would sleep here (0 <= 3299). A failed ADC must not lock the owner out.
        ESP_LOGW(TAG, "no battery reading (%d mV): empty-battery check skipped", mv);
    } else if (mv <= BATT_EMPTY_MV) {
        ESP_LOGW(TAG, "battery empty (%d mV <= %d mV) and no charger: back to deep sleep",
                 mv, BATT_EMPTY_MV);
        boot_sleep();
        return;
    }
    if (!by_motion) return;

    // Motion wake with a good battery and no charger.
    wait_ms(10);
    // Stock tests key_press_last (0x3fc9f301), which its GPIO3 / GPIO9 ISR sets when
    // the line is low; the levels are sampled here instead.
    bool button = gpio_get_level(HW_PIN_BUTTON) == 0;
    bool allowed = hal_anymotion_allowed();
    ESP_LOGI(TAG, "motion wake: button %d, any-motion window %d, gyro_wakeup_count %u",
             button, allowed, rtc->gyro_wakeup_count);
    if (button || oem_charger_present()) return;
    if (allowed && rtc->gyro_wakeup_count <= GYRO_WAKE_LIMIT) {
        if (rtc->gyro_wakeup_count < GYRO_WAKE_CAP) count_set(rtc->gyro_wakeup_count + 1);   // 0x4201b714
        return;
    }
    if (allowed) {
        ESP_LOGW(TAG, "motion wake over the limit (count %u > %d): back to deep sleep, motion wake off",
                 rtc->gyro_wakeup_count, GYRO_WAKE_LIMIT);
    } else {
        ESP_LOGW(TAG, "motion wake but the %d ms any-motion window has passed: back to deep sleep, motion wake off",
                 ANYMOTION_WINDOW_MS);
    }
    boot_sleep();
}

// Boot-time dispatch: app_main 0x4200c034 (wake cause) and the parts of
// brush_gpio_cfg 0x4200d77c that are not another driver's. Call it after
// nvs_flash_init() and before the other drivers are initialised.
int oem_power_boot(void)
{
    oem_rtc_t *rtc = hw_power_rtc();
    int cause = OEM_WAKE_COLD;
    bool by_motion = false;       // stock woke_by_ext0 0x3fca2884

    switch (hw_emulated() ? ESP_SLEEP_WAKEUP_UNDEFINED : esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
        uint64_t st = esp_sleep_get_ext1_wakeup_status();
        int pin = st ? __builtin_ffsll((long long)st) - 1 : -1;
        if (pin == HW_PIN_BUTTON) {
            cause = OEM_WAKE_BUTTON;
            count_set(0);                             // key_wakeup_clear_gyro_count 0x4201b740
        } else {
            cause = OEM_WAKE_CHARGER;                 // GPIO9, or no pin reported
        }
        ESP_LOGI(TAG, "wake from deep sleep: EXT1 GPIO %d (%s)", pin,
                 cause == OEM_WAKE_BUTTON ? "button" : "charger");
        break;
    }
    case ESP_SLEEP_WAKEUP_EXT0:
        cause = OEM_WAKE_MOTION;
        by_motion = true;
        ESP_LOGI(TAG, "wake from deep sleep: EXT0 GPIO8 (motion), gyro_wakeup_count %u", rtc->gyro_wakeup_count);
        break;
    case ESP_SLEEP_WAKEUP_TIMER:                      // never armed
        cause = OEM_WAKE_TIMER;
        ESP_LOGI(TAG, "wake from deep sleep: timer");
        break;
    default: {
        // The RTC copy did not survive this reset: take the count from NVS. (Stock
        // reads NVS on every boot, 0x4201b6dc; after a deep sleep the two agree.)
        uint8_t v;
        if (count_nvs_read(&v)) rtc->gyro_wakeup_count = v;
        ESP_LOGI(TAG, "not a deep sleep reset, gyro_wakeup_count %u", rtc->gyro_wakeup_count);
        break;
    }
    }
    if (by_motion) {                                  // anymotion_arm 0x40377c64
        s_anymotion_tick = xTaskGetTickCount();
        s_anymotion_armed = true;
    }

    // Pins parked with gpio_hold_en before the last sleep: the hold survives the
    // wake. Their owners configure them afterwards (IMU SPI pins: hw_imu.c, LCD SPI
    // pins: display driver, GPIO26: oem_charge_allow).
    static const uint8_t held[] = {
        HW_PIN_BUS_SCL, HW_PIN_BUS_SDA, HW_PIN_BACKLIGHT, HW_PIN_LCD_RST, HW_PIN_LCD_DC, HW_PIN_LCD_PWR,
        HW_PIN_LED1, HW_PIN_LED2, HW_PIN_LED3, HW_PIN_LED4, HW_PIN_TOUCH_RDY, HW_PIN_BUTTON,
    };
    for (unsigned i = 0; i < sizeof held; i++) gpio_hold_dis(held[i]);
    isr_service();
    // The button must be readable for the motion-wake check below even if its driver
    // is not up yet: input with pull-up, as stock configures it (interrupt settings
    // are left to the button driver).
    gpio_set_direction(HW_PIN_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HW_PIN_BUTTON, GPIO_PULLUP_ONLY);
    pin_cfg(HW_PIN_MOTION_INT, GPIO_MODE_INPUT, false, false, GPIO_INTR_DISABLE);   // edge type: oem_motion_irq
    pin_cfg(HW_PIN_NTC_ADC, GPIO_MODE_INPUT, false, false, GPIO_INTR_DISABLE);      // unused by stock too

    oem_imu_normal();
    if (!hw_emulated()) boot_sleep_checks(by_motion);

    // The boot continues. GPIO37 low = awake (stock drives it at the end of brush_gpio_cfg).
    pin_out(HW_PIN_LCD_PWR, 0);
    pm_init();
    return cause;
}
