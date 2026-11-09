#include <string.h>

#include "purr_abi.h"
#include "purr_cfg.h"
#include "purr_reason.h"
#include "purr_verify.h"
#include "testkit.h"

static void test_mapping(void)
{
    CHECK_EQ(purr_reason_from_verify(PURR_V_OK), 0);
    CHECK_EQ(purr_reason_from_verify(PURR_V_NO_KEY), PURR_RR_UNSIGNED);
    CHECK_EQ(purr_reason_from_verify(PURR_V_REVOKED), PURR_RR_UNSIGNED);
    CHECK_EQ(purr_reason_from_verify(PURR_V_ROLE_MISMATCH), PURR_RR_UNSIGNED);
    CHECK_EQ(purr_reason_from_verify(PURR_V_BAD_HASH), PURR_RR_ALTERED);
    CHECK_EQ(purr_reason_from_verify(PURR_V_BAD_SIGNATURE), PURR_RR_ALTERED);
    CHECK_EQ(purr_reason_from_verify(PURR_V_BELOW_FLOOR), PURR_RR_ROLLBACK);
    CHECK_EQ(purr_reason_from_verify(PURR_V_BAD_MAGIC), PURR_RR_DAMAGED);
    CHECK_EQ(purr_reason_from_verify(PURR_V_IO_ERROR), PURR_RR_DAMAGED);
    CHECK_EQ(purr_reason_from_verify(9999), PURR_RR_DAMAGED);          /* an unknown result fails closed */
}

static void test_text(void)
{
    for (int i = 0; i < PURR_RR_COUNT; i++) {
        CHECK(purr_reason_text(i) != NULL && strlen(purr_reason_text(i)) > 10);
    }
    CHECK(purr_reason_text(-1) == NULL && purr_reason_text(PURR_RR_COUNT) == NULL);

    const char *lines[4];
    CHECK_EQ(purr_reason_lines(0, lines, 4), 0);
    CHECK_EQ(purr_reason_lines(PURR_RR_ALTERED | PURR_RR_REPEATED, lines, 4), 2);
    CHECK(lines[0] == purr_reason_text(1) && lines[1] == purr_reason_text(4));          /* fixed order */
    CHECK_EQ(purr_reason_lines(0xFFFF, lines, 3), 3);                                      /* bounded */
}

static void test_full_restore(void)
{
    CHECK_EQ(purr_reason_needs_full_restore(0), 0);
    CHECK_EQ(purr_reason_needs_full_restore(PURR_RR_REPEATED | PURR_RR_REQUESTED | PURR_RR_CONFIG), 0);
    CHECK_EQ(purr_reason_needs_full_restore(PURR_RR_ALTERED), 1);
    CHECK_EQ(purr_reason_needs_full_restore(PURR_RR_REPEATED | PURR_RR_NO_SYSTEM), 1);
}

static void test_first_run(void)
{
    CHECK_EQ(purr_reason_is_first_run(0), 0);
    CHECK_EQ(purr_reason_is_first_run(PURR_RR_BLANK | PURR_RR_NO_SYSTEM), 1);
    CHECK_EQ(purr_reason_is_first_run(PURR_RR_BLANK | PURR_RR_NO_SYSTEM | PURR_RR_REQUESTED), 1);
    CHECK_EQ(purr_reason_is_first_run(PURR_RR_BLANK | PURR_RR_DAMAGED), 0);            /* half erased: a failure */
    CHECK_EQ(purr_reason_is_first_run(PURR_RR_BLANK | PURR_RR_ALTERED), 0);
    CHECK_EQ(purr_reason_is_first_run(PURR_RR_NO_SYSTEM), 0);                           /* missing is not the same as blank */
    CHECK_EQ(purr_reason_needs_full_restore(PURR_RR_BLANK), 1);
}

static void test_handoff_reasons(void)
{
    purr_handoff_t h;
    purr_handoff_make(&h, PURR_BOOT_AUTO_REINSTALL, 0xFF, 0, 0x200, PURR_SECURE_ENFORCE);
    CHECK_EQ(h.recovery_reasons, 0);
    CHECK_EQ(purr_handoff_valid(&h), 1);
    purr_handoff_set_reasons(&h, PURR_RR_ALTERED | PURR_RR_REPEATED);
    CHECK_EQ(h.recovery_reasons, PURR_RR_ALTERED | PURR_RR_REPEATED);
    CHECK_EQ(purr_handoff_valid(&h), 1);                        /* resealed */
    h.recovery_reasons = 0;                                     /* a flipped flag is caught by the CRC */
    CHECK_EQ(purr_handoff_valid(&h), 0);
    CHECK_EQ(sizeof(h), 20u);
}

int main(void)
{
    test_mapping();
    test_text();
    test_full_restore();
    test_first_run();
    test_handoff_reasons();
    TK_DONE("test_reason");
}
