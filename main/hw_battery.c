#include "hardware.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "hw_bat";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ok;

void hw_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { ESP_LOGE(TAG, "adc unit"); return; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_11, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(s_adc, HW_ADC_VBAT_CH, &cc);
    adc_cali_curve_fitting_config_t cfc = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_11, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfc, &s_cali) != ESP_OK)
        ESP_LOGW(TAG, "adc cali unavailable; mV will be approximate");
    s_ok = true;
    ESP_LOGI(TAG, "battery ADC ready (ADC1_CH%d)", HW_ADC_VBAT_CH);
}

int hw_battery_mv(void)
{
    if (!s_ok) return -1;
    int raw1 = 0, raw2 = 0;
    adc_oneshot_read(s_adc, HW_ADC_VBAT_CH, &raw1);
    adc_oneshot_read(s_adc, HW_ADC_VBAT_CH, &raw2);
    int raw = (raw1 + raw2) / 2;
    int mv = 0;
    if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    else mv = raw * 3300 / 4095;   // rough fallback
    return mv * 2;                  // ×2 external divider (confirmed in stock FW)
}

// Approximate V->% (exact stock table not recovered; linear 3.30–4.20 V).
int hw_battery_pct(void)
{
    int mv = hw_battery_mv();
    if (mv < 0) return -1;
    int pct = (mv - 3300) * 100 / (4200 - 3300);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
