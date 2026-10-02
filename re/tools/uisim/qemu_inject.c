// Scripted input for a QEMU run of the firmware (test only, never part of a release).
//
// QEMU cannot press the button, and without input the image only shows its boot,
// idle and sleep paths. This file plays a script: button codes are delivered the way
// the button driver does it (oem_button_push + hal_event_post) from a real interrupt
// (the FreeRTOS tick hook on CPU 0), remote commands and UI messages from a task of
// its own, the way the web server / MQTT / the gesture decoder do it. It does nothing
// on real hardware (hw_emulated() is false there), but do not flash such a build.
//
// Use: build it in a copy of the project, so the real tree and its build stay as they are.
//   mkdir P; cp CMakeLists.txt sdkconfig sdkconfig.defaults partitions.csv P/; cp -r main P/main
//   cp re/tools/uisim/qemu_inject.c P/main/zz_qemu_inject.c
//   echo 'target_link_libraries(${COMPONENT_LIB} INTERFACE "-u qemu_inject_anchor")' >> P/main/CMakeLists.txt
//   (cd P && idf.py -B build build); mkqemu.py P/build FLASH.bin ...; run QEMU; grep "inject\|oem\|glue" UART.log
// The -u is needed: nothing references this file, so the linker would drop it. The
// script below expects a brush that boots to the mode page (NVS with a sys_config whose
// byte 0 is 1 or 2); on a blank NVS the first events hit the pairing guide instead.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "hardware.h"
#include "oem_api.h"
#include "oem_hal.h"

static const char *TAG = "inject";

enum { BTN, R_BRUSH, R_MODE, R_STRENGTH, R_ACTIVITY, UI_MSG, FACTORY };
static const struct { uint32_t at_ms; uint8_t what, arg; const char *note; } SCRIPT[] = {
    {  10000, BTN, 0,        "short press on the mode page: start a session" },
    {  14000, R_STRENGTH, 4, "remote strength 4 (live in mode 5)" },
    {  20000, BTN, 0,        "short press: pause" },
    {  23000, BTN, 0,        "short press: resume" },
    {  40000, R_BRUSH, 0,    "remote stop" },
    {  45000, R_MODE, 3,     "remote mode 3" },
    {  64000, BTN, 1,        "2 s hold on the mode page: touch lock on" },
    {  68000, UI_MSG, 10,    "swipe up while locked: popup" },
    {  72000, BTN, 1,        "2 s hold: touch lock off" },
    {  76000, UI_MSG, 10,    "swipe up: next mode page" },
    { 125000, R_BRUSH, 1,    "remote start (screen off by now): wake + session" },
    { 140000, R_BRUSH, 0,    "remote stop" },
    { 185000, R_ACTIVITY, 0, "client activity in the screen-off stage: window restarts" },
    { 190000, BTN, 0,        "short press in the screen-off stage: wake" },
    { 262000, FACTORY, 0,    "8 s hold: factory reset (button code 4 + UI event), the brush restarts" },
};

#define REQ_FACTORY 0x80
static volatile uint8_t s_isr_req;       // button code + 1 (or REQ_FACTORY) for the tick hook to deliver
static volatile uint32_t s_isr_done;

// Tick interrupt on CPU 0: what the GPIO3 interrupt of hw_button.c does on a release,
// and what its 8 s timer does (there from the esp_timer task, here from the interrupt).
static void tick_hook(void)
{
    uint8_t req = s_isr_req;
    if (!req) return;
    s_isr_req = 0;
    oem_button_push(req == REQ_FACTORY ? 4 : (uint8_t)(req - 1));
    hal_event_post(OEM_EV_BUTTON);
    if (req == REQ_FACTORY) hal_ui_event_post(OEM_UIEV_FACTORY);
    s_isr_done++;
}

static void inject_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!hw_emulated()) { vTaskDelete(NULL); return; }
    esp_register_freertos_tick_hook_for_cpu(tick_hook, 0);
    ESP_LOGW(TAG, "scripted input active (%u events)", (unsigned)(sizeof SCRIPT / sizeof SCRIPT[0]));
    for (unsigned i = 0; i < sizeof SCRIPT / sizeof SCRIPT[0]; i++) {
        TickType_t now = xTaskGetTickCount(), at = pdMS_TO_TICKS(SCRIPT[i].at_ms);
        if (at > now) vTaskDelay(at - now);
        ESP_LOGW(TAG, "inject: %s", SCRIPT[i].note);
        uint8_t a = SCRIPT[i].arg;
        switch (SCRIPT[i].what) {
        case BTN:        s_isr_req = (uint8_t)(a + 1); break;
        case R_BRUSH:    oem_remote_brushing(a); break;
        case R_MODE:     oem_remote_mode(a); break;
        case R_STRENGTH: oem_remote_strength(a); break;
        case R_ACTIVITY: oem_net_activity(); break;
        case UI_MSG:     hal_lock(); oem_ui_post(a, NULL, 0); hal_unlock(); break;
        case FACTORY:    s_isr_req = REQ_FACTORY; break;
        }
    }
    vTaskDelay(2);
    ESP_LOGW(TAG, "script done (%u events delivered from the tick interrupt)", (unsigned)s_isr_done);
    vTaskDelete(NULL);
}

// The linker keeps this file because of "-u qemu_inject_anchor"; the constructor
// runs before the scheduler starts, where creating a task is allowed.
int qemu_inject_anchor;
__attribute__((constructor)) static void inject_ctor(void)
{
    xTaskCreate(inject_task, "qemu_inject", 4096, NULL, 5, NULL);
}
