#include "hardware.h"
#include "oem_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// Battery voltage: GPIO1 = ADC1 channel 0 behind a 1:1 divider, 12 bit, 11 dB
// (re/spec/led_battery_charge.md 5.1; stock init in brush_gpio_cfg 0x4200d77c, read
// in 0x4200cc94).
//
// Stock converts with the legacy esp_adc_cal line-fitting characterisation
// (ESP_ADC_CAL_VAL_EFUSE_TP_FIT). IDF 5.1 on the ESP32-S3 offers curve fitting for
// the one-shot driver: the same eFuse calibration, plus a correction of the
// non-linearity at 11 dB. The result can differ from stock by a few mV, so the
// gauge thresholds (3299 / 3445 / 3999 / 4109 mV) sit that much off the stock ones.
//
// As in stock, without calibration every reading is 0 mV; the gauge treats that as
// "no reading" (see OEM_BATT_MV_MIN_VALID in oem_api.h). The NTC channel
// (GPIO10 / ADC1_CH9) is configured but never read by stock and is left alone.

#define VBAT_CHANNEL  ADC_CHANNEL_0     // GPIO1
#define EMULATED_MV   4000

static const char *TAG = "hw_bat";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_init_done;

void oem_batt_adc_init(void)
{
    if (s_init_done) return;
    s_init_done = true;
    if (hw_emulated()) return;

    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    esp_err_t e = adc_oneshot_new_unit(&unit, &s_adc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "adc unit: %d", (int)e); s_adc = NULL; return; }

    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_11, .bitwidth = ADC_BITWIDTH_12 };
    e = adc_oneshot_config_channel(s_adc, VBAT_CHANNEL, &chan);
    if (e != ESP_OK) ESP_LOGE(TAG, "adc channel: %d", (int)e);

    adc_cali_curve_fitting_config_t cal = {
        .unit_id  = ADC_UNIT_1,
        .chan     = VBAT_CHANNEL,
        .atten    = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_12,
    };
    e = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (e != ESP_OK) {
        s_cali = NULL;
        ESP_LOGE(TAG, "no ADC calibration (%d): battery voltage unavailable", (int)e);
    }
}

// batt_read_mv 0x4200cc94: one conversion, calibrated pin voltage times 2.
static int read_mv_once(void)
{
    if (!s_adc || !s_cali) return 0;
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, VBAT_CHANNEL, &raw) != ESP_OK) return 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return 0;
    return mv * 2;
}

// One gauge sample: two reads 10 ms apart, averaged. Stock waits another 10 ms after
// the second read (vTaskDelay(10) at 1 kHz); that wait has no effect and is left out.
// The tick is 100 Hz here, so the wait between the reads is two ticks (10..20 ms).
int oem_batt_mv_now(void)
{
    oem_batt_adc_init();
    if (hw_emulated()) return EMULATED_MV;

    int a = read_mv_once();
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    int b = read_mv_once();
    if (a <= 0 || b <= 0) return 0;      // failed read: report "no reading", not half a value
    return (a + b) >> 1;
}
