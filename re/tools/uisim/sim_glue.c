// Host test of the glue's Wi-Fi rules as the brush logic sees them (NOTES_glue.md 5).
//   cc -std=gnu11 -Wall -Wextra -I main re/tools/uisim/sim_glue.c main/oem_app.c main/oem_brush.c -o sim_glue
//   ./sim_glue [-v]
//
// wifi_mgr.c and the radio side of oem_glue.c run neither on the host nor in QEMU.
// What they do to the core is little, though: hal_wifi_has_ssid() changes its answer
// while the setup AP is up, and a few core functions are called at Wi-Fi events. This
// file replays exactly those calls against the real oem_app.c / oem_brush.c in the
// harness of sim_app.c (virtual time, fakes for the other modules), and for each rule
// also what happens without it. It does not test wifi_mgr.c itself.
#define main sim_app_main               // the harness and its fakes, not its scenario runner
#include "sim_app.c"
#undef main

// The STA_CONNECTED branch of on_wifi() in wifi_mgr.c. old = true is the first
// version of that handler: the stock side effects at every connection, and the wake
// script also on the charger.
static bool s_greeted;
static void sta_connected(bool old)
{
    hal_lock();
    g_oem.wifi_status = 1;
    bool first = !s_greeted || old;
    s_greeted = true;
    if (first) {
        if (!g_oem.session_active) oem_idle_timeout(60);
        if (g_oem.batt_pct && (old || g_oem.power_state == OEM_PWR_BATTERY)) oem_led_set(1, 0, 0);
    }
    hal_unlock();
    if (first) hal_event_post(OEM_EV_BLE_WAKE);
}
static void sta_disconnected(void) { hal_lock(); g_oem.wifi_status = 2; hal_unlock(); }

// A link that comes back every 20 s (first connection 3 s after boot).
static void flapping(bool old, uint32_t until_ms)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(3000);
    note("STA_CONNECTED at 3 s, then a disconnect and a reconnect every 20 s");
    sta_connected(old);
    for (uint32_t t = 23000; t < until_ms && !g_deep_sleep; t += 20000) {
        run_until(t);
        sta_disconnected();
        sta_connected(old);
    }
    run_until(until_ms);
}

static void sc_sta_reconnects(void)
{
    flapping(false, 100000);
    int m = expect("timer BLE_TIMEOUT start 30000 ms", 0);
    CHECK(g_log_ms[m] < 40000, "screen off at %.2f s, as without Wi-Fi events", g_log_ms[m] / 1000.0);
    CHECK(count("power stay_alive", m) == 0, "no reconnect wakes the screen");
    expect_at("DEEP SLEEP", m, g_log_ms[m] + 30000 + 150, 20);
}

static void sc_sta_reconnects_old(void)
{
    flapping(true, 400000);
    CHECK(!g_deep_sleep, "(first version) no deep sleep within 400 s");
    expect_none("lcd sleep", 0);            // the idle timer starts over every 20 s: the screen never goes off
}

// On the dock the backlight goes off 30 s after docking and stays off. A connection
// after that (a reconnect in the first version; with the fix only a first connection
// that comes late: the network was down, or new credentials) must not start the wake
// script: it fades the backlight in, and delay_to_close_screen() never ends it.
static int docked_connect(bool old)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(10000);
    note("on the dock at 10 s; STA_CONNECTED at 100 s, 60 s after the backlight went off");
    g_charger = true;
    run_until(100000);
    int m = expect("led 4 -> off", find("ui post 93", 0));
    uint32_t t_off = m >= 0 ? g_log_ms[m] : 0;
    CHECK(t_off > 39000 && t_off < 42000, "backlight off 30 s after docking (%.2f s)", t_off / 1000.0);
    m = mark();
    sta_connected(old);
    run_until(400000);
    return m;
}

static void sc_dock_late_connect(void)
{
    int m = docked_connect(false);
    expect_none("led script", m);
    expect_none("led 4 -> ON", m);
    expect_none("DEEP SLEEP", 0);
}

static void sc_dock_late_connect_old(void)
{
    int m = docked_connect(true);
    expect("led script 0, then led 1 = 0", m);
    expect_none("led 4 -> off", m);         // nothing switches the backlight off in the next 300 s
}

// Boot on the dock, Wi-Fi connects after 2 s: the charger is detected with the gauge's
// first measurement (3 s), so the wake script still starts; the charger handling aborts
// it, and the backlight goes off 30 s later as usual.
static void sc_dock_boot_connect(void)
{
    g_charger = true;
    boot(2, 80, OEM_WAKE_CHARGER);
    run_until(2000);
    note("STA_CONNECTED at 2 s, before the charger is detected");
    int m = mark();
    sta_connected(false);
    run_until(100000);
    int a = expect("led script 0, then led 1 = 0", m);
    a = expect("led script aborted", a);
    a = expect("led script 2, then led 2 = 3", a);
    int off = expect("led 4 -> off", find("ui post 93", a));
    uint32_t t_off = off >= 0 ? g_log_ms[off] : 0;
    CHECK(t_off > 30000 && t_off < 36000, "backlight off 30 s after the charger screen (%.2f s)", t_off / 1000.0);
    expect_none("led 4 -> ON", off);
}

// The stored network cannot be joined: the setup AP comes up 50 s after boot, in the
// screen-off stage. From then on hal_wifi_has_ssid() is false, and wifi_mgr.c calls
// oem_net_activity() for the AP coming up, for a station joining and (web_server.c)
// for a load of the page.
static void sc_setup_ap(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(50000);
    int m = expect("timer BLE_TIMEOUT start 30000 ms", 0);
    note("setup AP up at 50 s");
    g_ssid = false;
    oem_net_activity();
    run_until(120000);
    int w = expect_at("timer BLE_TIMEOUT start 120000 ms", m + 1, 50001, 10);
    expect_none("DEEP SLEEP", 0);
    note("a phone joins the AP at 120 s and loads the page at 150 s");
    oem_net_activity();
    run_until(150000);
    oem_net_activity();
    run_until(300000);
    expect_at("DEEP SLEEP", w, 150001 + 120000 + 150, 20);
    CHECK(count("power stay_alive", m) == 0, "the screen stays off");
    CHECK(g_lock_depth == 0, "lock balanced");
}

// Without the report for the AP coming up, the 30 s window that is already running
// ends the AP: the new answer of hal_wifi_has_ssid() only counts at the next start.
static void sc_setup_ap_unreported(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(50000);
    int m = expect("timer BLE_TIMEOUT start 30000 ms", 0);
    note("setup AP up at 50 s, hal_wifi_has_ssid() false, nothing reported");
    g_ssid = false;
    run_until(120000);
    expect_at("DEEP SLEEP", m, g_log_ms[m] + 30000 + 150, 20);
}

// The setup AP is already up when the screen goes off (a brush without credentials,
// or one that was used for a minute): the long window from the start.
static void sc_setup_ap_before_screen_off(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(20000);
    note("setup AP up at 20 s, screen still on: the report does nothing");
    g_ssid = false;
    oem_net_activity();
    run_until(170000);
    int m = expect("timer BLE_TIMEOUT start 120000 ms", 0);
    CHECK(g_log_ms[m] < 40000, "screen off on schedule (%.2f s)", g_log_ms[m] / 1000.0);
    expect_none("timer BLE_TIMEOUT start 30000 ms", 0);
    expect_at("DEEP SLEEP", m, g_log_ms[m] + 120000 + 150, 20);
}

static const struct { const char *name; void (*fn)(void); const char *title; } GLUE_SCEN[] = {
    { "sta_reconnects", sc_sta_reconnects, "Wi-Fi link that reconnects every 20 s: side effects once, sleep on schedule" },
    { "sta_reconnects_old", sc_sta_reconnects_old, "counter-check, first version: side effects at every reconnect" },
    { "dock_late_connect", sc_dock_late_connect, "on the dock, connection after the backlight time-out: no wake script" },
    { "dock_late_connect_old", sc_dock_late_connect_old, "counter-check, first version: the wake script runs and nothing ends it" },
    { "dock_boot_connect", sc_dock_boot_connect, "boot on the dock, connection before the charger is detected" },
    { "setup_ap", sc_setup_ap, "setup AP comes up in the screen-off stage: 120 s window, restarted by a join and a page load" },
    { "setup_ap_unreported", sc_setup_ap_unreported, "counter-check: AP up without the activity report" },
    { "setup_ap_before_screen_off", sc_setup_ap_before_screen_off, "setup AP up before the screen goes off" },
};

int main(int argc, char **argv)
{
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = true;
        else only = argv[i];
    }
    int failed = 0, ran = 0;
    for (size_t i = 0; i < sizeof GLUE_SCEN / sizeof GLUE_SCEN[0]; i++) {
        if (only && strncmp(GLUE_SCEN[i].name, only, strlen(only)) != 0) continue;
        printf("=== %s: %s\n", GLUE_SCEN[i].name, GLUE_SCEN[i].title);
        fflush(stdout);
        pid_t pid = fork();                 // every scenario from a fresh boot, as in sim_app.c
        if (pid == 0) {
            GLUE_SCEN[i].fn();
            printf("  %d checks, %d failed\n", g_checks, g_fail);
            fflush(stdout);
            _exit(g_fail ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        ran++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed++;
    }
    printf("\n%d scenarios, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
