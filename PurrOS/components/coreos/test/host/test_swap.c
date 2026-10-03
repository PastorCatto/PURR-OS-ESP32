#include <string.h>

#include "purr_abi.h"
#include "purr_swap.h"
#include "testkit.h"

static purr_swap_action_t run(uint8_t state, uint8_t attempts, uint8_t max_attempts)
{
    purr_update_t u = {PURR_COMP_COREOS, state, attempts, 0, 0};
    return purr_swap_decide(&u, max_attempts);
}

static void test_requested_does_the_swap(void)
{
    CHECK_EQ(run(PURR_UPD_REQUESTED, 0, 3), PURR_SWAP_DO_SWAP);
    CHECK_EQ(run(PURR_UPD_REQUESTED, 2, 3), PURR_SWAP_DO_SWAP);   /* attempts irrelevant here */
}

static void test_moving_resumes(void)
{
    CHECK_EQ(run(PURR_UPD_MOVING, 0, 3), PURR_SWAP_RESUME_MOVE);
}

static void test_unconfirmed_retries_then_rolls_back(void)
{
    CHECK_EQ(run(PURR_UPD_UNCONFIRMED, 0, 3), PURR_SWAP_RETRY);
    CHECK_EQ(run(PURR_UPD_UNCONFIRMED, 1, 3), PURR_SWAP_RETRY);
    CHECK_EQ(run(PURR_UPD_UNCONFIRMED, 2, 3), PURR_SWAP_RETRY);
    CHECK_EQ(run(PURR_UPD_UNCONFIRMED, 3, 3), PURR_SWAP_ROLLBACK);
    CHECK_EQ(run(PURR_UPD_UNCONFIRMED, 4, 3), PURR_SWAP_ROLLBACK);   /* past the limit too */
}

static void test_quiet_states_do_nothing(void)
{
    CHECK_EQ(run(PURR_UPD_NONE, 0, 3), PURR_SWAP_NOTHING);
    CHECK_EQ(run(PURR_UPD_STAGED, 0, 3), PURR_SWAP_NOTHING);
    CHECK_EQ(run(PURR_UPD_CONFIRMED, 0, 3), PURR_SWAP_NOTHING);
    CHECK_EQ(run(PURR_UPD_FAILED, 5, 3), PURR_SWAP_NOTHING);
}

static void test_filenames(void)
{
    CHECK(purr_swap_filename(PURR_COMP_COREOS) != 0);
    CHECK_EQ(strcmp(purr_swap_filename(PURR_COMP_COREOS), "coreos.kitt"), 0);
    CHECK_EQ(strcmp(purr_swap_filename(PURR_COMP_APPMANAGER), "appmanager.kitt"), 0);
    CHECK_EQ(strcmp(purr_swap_filename(PURR_COMP_RUNTIME), "runtime.kitt"), 0);
    CHECK_EQ(strcmp(purr_swap_filename(PURR_COMP_DEVBUNDLE), "devbundle.kitt"), 0);

    /* Partition-level components: not this mechanism's job. */
    CHECK(purr_swap_filename(PURR_COMP_KERNEL) == 0);
    CHECK(purr_swap_filename(PURR_COMP_KITTENOS) == 0);
    CHECK(purr_swap_filename(PURR_COMP_LOADER) == 0);
    CHECK(purr_swap_filename(PURR_COMP_BOOTPKG) == 0);
    CHECK(purr_swap_filename(0) == 0);
}

int main(void)
{
    test_requested_does_the_swap();
    test_moving_resumes();
    test_unconfirmed_retries_then_rolls_back();
    test_quiet_states_do_nothing();
    test_filenames();
    TK_DONE("test_swap");
}
