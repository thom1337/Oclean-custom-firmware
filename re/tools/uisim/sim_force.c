// Host check of the brushing-force algorithm (main/oem_force.c), the port of the
// Awinic library the stock firmware uses for the AW8686X.
//   cc -std=gnu11 -Wall -Wextra -ffp-contract=off -I main re/tools/uisim/sim_force.c -o sim_force
//   ./sim_force                 self test (calibration record, temperature coefficient,
//                               a press / release scenario)
//   python3 re/tools/uisim/check_force.py
//                               differential test: runs the ORIGINAL machine code of
//                               the stock image in an Xtensa interpreter
//                               (re/tools/xt_emu.py) and this port on the same sample
//                               streams and compares the whole 236-byte state after
//                               every sample. It drives this program in pipe mode:
//   ./sim_force pipe            stdin:  "I noise coef pos_timeout neg_timeout thr_press thr_release
//                                          thr_track quiet_lim thr_heavy heavy_n"  (init; the stock
//                                          values are 60000 60000 20 10 10 20 1600 8)
//                                       "S value"                                  (one sample)
//                               stdout: per sample "force <state as hex>"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../main/oem_force.c"      // included, not linked: the test sets parameters in the state

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int pipe_mode(void)
{
    char line[160];
    while (fgets(line, sizeof line, stdin)) {
        int a, p[10];
        if (line[0] == 'I' && sscanf(line + 1, "%d %d %d %d %d %d %d %d %d %d", &p[0], &p[1], &p[2], &p[3], &p[4],
                                     &p[5], &p[6], &p[7], &p[8], &p[9]) == 10) {
            oem_force_init((uint16_t)p[0], (uint16_t)p[1]);
            s.pos_timeout = (uint32_t)p[2];
            s.neg_timeout = (uint32_t)p[3];
            s.thr_press = (uint16_t)p[4];
            s.thr_release = (int16_t)p[5];
            s.thr_track = (uint16_t)p[6];
            s.quiet_lim = (uint16_t)p[7];
            s.thr_heavy = (int16_t)p[8];
            s.heavy_n = (uint16_t)p[9];
        } else if (line[0] == 'S' && sscanf(line + 1, "%d", &a) == 1) {
            int f = oem_force_step((int16_t)a);
            printf("%d ", f);
            const uint8_t *p = oem_force_state();
            for (int i = 0; i < OEM_FORCE_STATE_SIZE; i++) printf("%02x", p[i]);
            printf("\n");
        }
    }
    return 0;
}

static void make_record(uint8_t rec[16], uint16_t noise, uint16_t coef)
{
    memset(rec, 0, 16);
    rec[0] = ':'; rec[1] = 0xA0; rec[2] = 6;
    rec[3] = (uint8_t)noise; rec[4] = (uint8_t)(noise >> 8);
    rec[5] = (uint8_t)coef;  rec[6] = (uint8_t)(coef >> 8);
    rec[7] = 0x5A; rec[8] = 0x5A;
    uint32_t sum = 0;
    for (int i = 3; i < 9; i++) sum += rec[i];
    rec[9] = (uint8_t)sum; rec[10] = (uint8_t)(sum >> 8); rec[11] = (uint8_t)(sum >> 16); rec[12] = (uint8_t)(sum >> 24);
    rec[14] = '\r'; rec[15] = '\n';
}

static void test_calibration(void)
{
    printf("calibration record\n");
    uint8_t rec[16];
    uint16_t noise, coef;
    make_record(rec, 7, 31);
    CHECK(oem_force_parse_cal(rec, &noise, &coef) && noise == 7 && coef == 31, "valid record: %u %u", noise, coef);
    rec[5] ^= 1;
    CHECK(!oem_force_parse_cal(rec, &noise, &coef) && noise == 9 && coef == 23, "bad checksum -> defaults");
    make_record(rec, 7, 31); rec[8] = 0;
    for (int i = 9; i < 13; i++) rec[i] = 0;
    uint32_t sum = 0; for (int i = 3; i < 9; i++) sum += rec[i];
    rec[9] = (uint8_t)sum; rec[10] = (uint8_t)(sum >> 8);
    CHECK(!oem_force_parse_cal(rec, &noise, &coef) && coef == 23, "bad magic -> defaults");
    memset(rec, 0, 16);
    CHECK(!oem_force_parse_cal(rec, &noise, &coef) && noise == 9 && coef == 23, "empty record -> defaults");
    make_record(rec, 7, 31); rec[14] = 0;
    CHECK(!oem_force_parse_cal(rec, &noise, &coef), "missing CR -> defaults");
}

static void test_temperature(void)
{
    printf("temperature coefficient\n");
    CHECK(oem_force_temp_coef(25, 0) == 1.0f, "no base -> 1");
    CHECK(oem_force_temp_coef(25, 25) == 1.0f, "same temperature -> 1");
    CHECK(oem_force_temp_coef(35, 25) == (float)1.1, "+10 C -> 1.10 (%f)", oem_force_temp_coef(35, 25));
    CHECK(oem_force_temp_coef(15, 25) == (float)0.9, "-10 C -> 0.90");
    CHECK(oem_force_temp_coef(60, 25) == 1.25f, "limit 1.25");
    CHECK(oem_force_temp_coef(-5, 25) == 0.75f, "limit 0.75");
    CHECK(oem_force_scale(400, 1.0f) == 400 && oem_force_scale(400, 1.25f) == 500 &&
          oem_force_scale(-30, 1.0f) == 0 && oem_force_scale(399, 0.75f) == 299, "scaling");
    CHECK(oem_force_scale(32000, 1.25f) == 0, "16-bit overflow reads as 0, like stock");
}

// A plausible session: rest, brush pressed on for 5 s with ripple, release.
static void test_scenario(void)
{
    printf("press / release scenario (coef 23, noise 9)\n");
    oem_force_init(9, 23);
    const int zero = -1500;                 // ADC rest level
    int f = 0, n = 0;
    for (int i = 0; i < 100; i++, n++) f = oem_force_step((int16_t)(zero + (i % 3) - 1));
    CHECK(abs(oem_force_baseline() - zero) <= 2, "baseline %d after 2 s of rest (want %d)", oem_force_baseline(), zero);
    CHECK(f == 0, "force %d at rest", f);
    // 2000 counts * 23 / 100 = 460
    for (int i = 0; i < 250; i++, n++) {
        int ramp = i < 20 ? i * 100 : 2000;
        f = oem_force_step((int16_t)(zero + ramp + ((i * 7) % 11) - 5));
    }
    CHECK(f >= 455 && f <= 465, "force %d while pressed with 2000 counts (want about 460)", f);
    CHECK(oem_force_pressed(), "library press state set");
    CHECK(abs(oem_force_baseline() - zero) <= 2, "baseline %d held during the press", oem_force_baseline());
    for (int i = 0; i < 100; i++, n++) f = oem_force_step((int16_t)(zero + 3 + (i % 3) - 1));
    CHECK(f >= -1 && f <= 1, "force %d after release", f);
    CHECK(!oem_force_pressed(), "library press state cleared");
    // slow drift of the rest level (temperature): the baseline follows
    for (int i = 0; i < 1500; i++, n++) f = oem_force_step((int16_t)(zero + 3 + i / 10 + (i % 3) - 1));
    CHECK(abs(oem_force_baseline() - (zero + 3 + 149)) <= 6, "baseline %d follows a slow drift (want about %d)",
          oem_force_baseline(), zero + 152);
    CHECK(f >= -2 && f <= 2, "force %d during drift", f);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "pipe")) return pipe_mode();
    test_calibration();
    test_temperature();
    test_scenario();
    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail != 0;
}
