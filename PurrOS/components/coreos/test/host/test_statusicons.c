#include <string.h>

#include "purr_statusicons.h"
#include "testkit.h"

static void test_battery(void)
{
    /* the edges of Froyo's level list: 4, 14, 29, 49, 69, 89 */
    const int edges[][2] = {{0, 0}, {4, 0}, {5, 1}, {14, 1}, {15, 2}, {29, 2}, {30, 3}, {49, 3}, {50, 4}, {69, 4}, {70, 5}, {89, 5}, {90, 6}, {100, 6}};
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        CHECK_EQ(purr_si_battery_level(edges[i][0]), edges[i][1]);
    }
    CHECK_EQ(purr_si_battery_level(-5), 0);                 /* out of range clamps */
    CHECK_EQ(purr_si_battery_level(250), 6);
    CHECK(strcmp(purr_si_battery_asset(62), "st_b60") == 0);
    CHECK(strcmp(purr_si_battery_asset(3), "st_b0") == 0);
    CHECK(strcmp(purr_si_battery_asset(100), "st_b100") == 0);
    CHECK(strcmp(purr_si_battery_unknown_asset(), "st_bunk") == 0);
}

static void test_charge(void)
{
    CHECK_EQ(purr_si_charge_start(0), 0);
    CHECK_EQ(purr_si_charge_start(14), 0);
    CHECK_EQ(purr_si_charge_start(15), 1);
    CHECK_EQ(purr_si_charge_start(49), 2);
    CHECK_EQ(purr_si_charge_start(50), 3);
    CHECK_EQ(purr_si_charge_start(89), 4);
    CHECK_EQ(purr_si_charge_start(90), 5);

    /* from 40 percent: frames 2, 3, 4, 5, then back to 2 */
    int start = purr_si_charge_start(40), f = start;
    const int want[] = {2, 3, 4, 5, 2, 3};
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(f, want[i]);
        f = purr_si_charge_next(f, start);
    }
    CHECK_EQ(purr_si_charge_ms(2, 2), 2000);                /* the start frame is held longer */
    CHECK_EQ(purr_si_charge_ms(3, 2), 1000);
    CHECK_EQ(purr_si_charge_next(5, 5), 5);                 /* nearly full: one frame, it stays */
    CHECK_EQ(purr_si_charge_next(1, 3), 3);                 /* a frame below the start joins the loop */
    CHECK(strcmp(purr_si_charge_asset(4), "st_bc4") == 0);
    CHECK(strcmp(purr_si_charge_asset(99), "st_bc5") == 0);
}

static void test_wifi(void)
{
    CHECK_EQ(purr_si_wifi_level(-120), 0);
    CHECK_EQ(purr_si_wifi_level(-100), 0);
    CHECK_EQ(purr_si_wifi_level(-89), 1);                   /* (-89 + 100) / 11 = 1 */
    CHECK_EQ(purr_si_wifi_level(-90), 0);
    CHECK_EQ(purr_si_wifi_level(-78), 2);
    CHECK_EQ(purr_si_wifi_level(-67), 3);
    CHECK_EQ(purr_si_wifi_level(-62), 3);
    CHECK_EQ(purr_si_wifi_level(-56), 4);                   /* 44 / 11 = 4 */
    CHECK_EQ(purr_si_wifi_level(-55), 4);
    CHECK_EQ(purr_si_wifi_level(-30), 4);
    CHECK(strcmp(purr_si_wifi_asset(3), "st_w3") == 0);
    CHECK(strcmp(purr_si_wifi_asset(9), "st_w4") == 0);
    CHECK(strcmp(purr_si_wifi_asset(-1), "st_w0") == 0);
}

static void test_lora_level(void)
{
    CHECK_EQ(purr_si_lora_level(0, -60, 0), 0);                          /* nothing heard yet */
    CHECK_EQ(purr_si_lora_level(1, -60, 1000), 4);
    CHECK_EQ(purr_si_lora_level(1, -85, 1000), 4);                       /* the edges */
    CHECK_EQ(purr_si_lora_level(1, -86, 1000), 3);
    CHECK_EQ(purr_si_lora_level(1, -100, 1000), 3);
    CHECK_EQ(purr_si_lora_level(1, -101, 1000), 2);
    CHECK_EQ(purr_si_lora_level(1, -112, 1000), 2);
    CHECK_EQ(purr_si_lora_level(1, -113, 1000), 1);
    CHECK_EQ(purr_si_lora_level(1, -140, 1000), 1);                      /* heard at all is one bar */
    CHECK_EQ(purr_si_lora_level(1, -60, PURR_SI_LORA_STALE_MS), 4);      /* ten minutes exactly is still recent */
    CHECK_EQ(purr_si_lora_level(1, -60, PURR_SI_LORA_STALE_MS + 1), 0);  /* a stale packet says nothing */
}

static void test_service(void)
{
    CHECK(strcmp(purr_si_service_asset(4), "st_s4") == 0);
    CHECK(strcmp(purr_si_service_asset(0), "st_s0") == 0);
    CHECK(strcmp(purr_si_service_asset(-1), "st_snull") == 0);
    CHECK(strcmp(purr_si_service_asset(7), "st_s4") == 0);
}

int main(void)
{
    test_battery();
    test_charge();
    test_wifi();
    test_service();
    test_lora_level();
    TK_DONE("test_statusicons");
}
