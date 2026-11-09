#include "purr_resetnote.h"

#include <stdio.h>
#include <string.h>

static void put(char *out, size_t cap, const char *text)
{
    if (out == NULL || cap == 0) {
        return;
    }
    snprintf(out, cap, "%s", text);
}

int purr_resetnote_text(int why, int detail, int had_data, char *brief, size_t bcap, char *full, size_t fcap)
{
    char buf[400];
    switch (why) {
    case PURR_RESET_AUTO_FORMAT:
        put(brief, bcap, "Your data was erased by automatic recovery.");
        snprintf(buf, sizeof(buf),
                 "This device's storage was erased and set up again by the recovery system, because the file system would not mount (it reported damage, code %d)%s. "
                 "Accounts, saved Wi-Fi, settings and apps on the device are gone. You did not ask for this. If it happens again, tell the project.",
                 detail, had_data ? " although it held data" : "");
        put(full, fcap, buf);
        return 1;
    case PURR_RESET_USER_ERASE:
        put(brief, bcap, "Your data was erased when you reinstalled.");
        put(full, fcap, "You chose to erase everything when reinstalling, so the device's storage was wiped: accounts, saved Wi-Fi, settings and apps are gone.");
        return 1;
    case PURR_RESET_WEAKEN_WIPE:
        put(brief, bcap, "Your data was erased when developer mode was turned on.");
        put(full, fcap, "Developer mode was turned on with the option to erase your data first, so the storage was wiped before security was relaxed: accounts, saved Wi-Fi, settings and apps are gone.");
        return 1;
    case PURR_RESET_DEFAULT_OVER_HOMES:
        put(brief, bcap, "No account list was found, so the default account was made.");
        put(full, fcap,
            "No account list could be read at start-up, although user folders exist on this device, so the default administrator MEOW (password MEOW, in capitals) was created. "
            "Your files are probably still in /home. Sign in as MEOW, make your account again with exactly the same name, check Settings > Accounts, and change the MEOW password.");
        return 1;
    default:
        put(brief, bcap, "Something replaced the data on this device.");
        put(full, fcap, "The data on this device was replaced, and the reason was not recorded. Check your accounts, saved Wi-Fi and settings.");
        return 0;
    }
}
