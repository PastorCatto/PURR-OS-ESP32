#include "purr_abi.h"
#include "purr_decision.h"
#include "testkit.h"

static purr_decision_t run(uint8_t boot, purr_own_check_t own, uint8_t mode,
                           uint8_t recovery_image, uint8_t tried)
{
    purr_decision_in_t in = {boot, own, mode, recovery_image, tried};
    return purr_decide(&in);
}

/* The rows of the decision table in the spec, one by one. */
static void test_table_rows(void)
{
    /* verified, pass */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_PASS, PURR_SECURE_OFF, 0, 0), PURR_DECIDE_CONTINUE);
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_PASS, PURR_SECURE_WARN, 0, 0), PURR_DECIDE_CONTINUE);
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_PASS, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_CONTINUE);

    /* verified, fail */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_OFF, 0, 0), PURR_DECIDE_CONTINUE);
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_WARN, 0, 0), PURR_DECIDE_CONTINUE_WARN);
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_RECOVERY);

    /* failed_warned, any own check */
    for (int own = PURR_OWN_PASS; own <= PURR_OWN_UNSIGNED; own++) {
        CHECK_EQ(run(PURR_BOOT_FAILED_WARNED, (purr_own_check_t)own, PURR_SECURE_OFF, 0, 0), PURR_DECIDE_CONTINUE);
        CHECK_EQ(run(PURR_BOOT_FAILED_WARNED, (purr_own_check_t)own, PURR_SECURE_WARN, 0, 0), PURR_DECIDE_CONTINUE_WARN);
        CHECK_EQ(run(PURR_BOOT_FAILED_WARNED, (purr_own_check_t)own, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_RECOVERY);
    }

    /* none (stock bootloader) */
    CHECK_EQ(run(PURR_BOOT_NONE, PURR_OWN_PASS, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_CONTINUE);
    CHECK_EQ(run(PURR_BOOT_NONE, PURR_OWN_FAIL, PURR_SECURE_OFF, 0, 0), PURR_DECIDE_CONTINUE);
    CHECK_EQ(run(PURR_BOOT_NONE, PURR_OWN_FAIL, PURR_SECURE_WARN, 0, 0), PURR_DECIDE_CONTINUE_WARN);
    CHECK_EQ(run(PURR_BOOT_NONE, PURR_OWN_FAIL, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_RECOVERY);

    /* A dev build with no container header and secure mode off is allowed. */
    CHECK_EQ(run(PURR_BOOT_NONE, PURR_OWN_UNSIGNED, PURR_SECURE_OFF, 0, 0), PURR_DECIDE_CONTINUE);

    /* forced_recovery: the user asked to be here, so always continue. */
    for (int own = PURR_OWN_PASS; own <= PURR_OWN_UNSIGNED; own++) {
        for (int mode = PURR_SECURE_OFF; mode <= PURR_SECURE_ENFORCE; mode++) {
            CHECK_EQ(run(PURR_BOOT_FORCED_RECOVERY, (purr_own_check_t)own, (uint8_t)mode, 0, 0), PURR_DECIDE_CONTINUE);
        }
    }
}

static void test_recovery_rules(void)
{
    /* KittenOS never falls back to itself. */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_ENFORCE, 1, 0), PURR_DECIDE_PROMPT);
    CHECK_EQ(run(PURR_BOOT_FAILED_WARNED, PURR_OWN_PASS, PURR_SECURE_ENFORCE, 1, 0), PURR_DECIDE_PROMPT);
    /* Only one automatic restart, then a degraded shell. */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_ENFORCE, 0, 1), PURR_DECIDE_DEGRADED);
    /* The rules do not affect the soft modes. */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_WARN, 1, 1), PURR_DECIDE_CONTINUE_WARN);
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, PURR_SECURE_OFF, 1, 1), PURR_DECIDE_CONTINUE);
    /* KittenOS with nothing wrong just continues. */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_PASS, PURR_SECURE_ENFORCE, 1, 1), PURR_DECIDE_CONTINUE);
}

static void test_fail_closed(void)
{
    /* An unknown secure mode is treated as enforce. */
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, 7, 0, 0), PURR_DECIDE_RECOVERY);
    CHECK_EQ(run(PURR_BOOT_VERIFIED, PURR_OWN_FAIL, 255, 1, 0), PURR_DECIDE_PROMPT);
    /* An unknown boot state with a good own check continues, with a bad one it fails. */
    CHECK_EQ(run(99, PURR_OWN_PASS, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_CONTINUE);
    CHECK_EQ(run(99, PURR_OWN_FAIL, PURR_SECURE_ENFORCE, 0, 0), PURR_DECIDE_RECOVERY);
}

/*
 * Exhaustive: every combination of every input, checked against a second,
 * differently written implementation of the same table.
 */
static purr_decision_t expected(int boot, int own, int mode, int rec, int tried)
{
    int fail = (boot == PURR_BOOT_FAILED_WARNED) || (boot != PURR_BOOT_FORCED_RECOVERY && own != PURR_OWN_PASS);
    if (boot == PURR_BOOT_FORCED_RECOVERY || !fail) {
        return PURR_DECIDE_CONTINUE;
    }
    if (mode == PURR_SECURE_OFF) {
        return PURR_DECIDE_CONTINUE;
    }
    if (mode == PURR_SECURE_WARN) {
        return PURR_DECIDE_CONTINUE_WARN;
    }
    return rec ? PURR_DECIDE_PROMPT : (tried ? PURR_DECIDE_DEGRADED : PURR_DECIDE_RECOVERY);
}

static void test_exhaustive(void)
{
    int boots[] = {0, 1, 2, 3, 4, 5, 200};
    int modes[] = {0, 1, 2, 3, 100};
    int count = 0;

    for (unsigned b = 0; b < sizeof(boots) / sizeof(boots[0]); b++) {
        for (int own = 0; own <= 2; own++) {
            for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
                for (int rec = 0; rec <= 1; rec++) {
                    for (int tried = 0; tried <= 1; tried++) {
                        CHECK_EQ(run((uint8_t)boots[b], (purr_own_check_t)own, (uint8_t)modes[m], (uint8_t)rec, (uint8_t)tried),
                                 expected(boots[b], own, modes[m], rec, tried));
                        count++;
                    }
                }
            }
        }
    }
    CHECK_EQ(count, 7 * 3 * 5 * 2 * 2);
}

int main(void)
{
    test_table_rows();
    test_recovery_rules();
    test_fail_closed();
    test_exhaustive();
    TK_DONE("test_decision");
}
