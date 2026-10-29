/* The names and the look of this flavor of PURR OS.
 *
 * Every value has a default (stock PURR OS). A flavor overrides any of them from a header that `purr brand build`
 * writes from its brand.toml: compile with -DPURR_BRAND_HEADER='"path/to/purr_brand.h"'. Strings must be plain
 * ASCII without quotes or backslashes. See sdk/DESIGN.md, "Branding".
 */
#ifndef PURR_BRAND_H
#define PURR_BRAND_H

#ifdef PURR_BRAND_HEADER
#include PURR_BRAND_HEADER
#endif

#ifndef PURR_BRAND_PRODUCT
#define PURR_BRAND_PRODUCT "PURR OS"        /* the full system: boot menu, About, messages */
#endif
#ifndef PURR_BRAND_RECOVERY
#define PURR_BRAND_RECOVERY "KittenOS"      /* the recovery system */
#endif
#ifndef PURR_BRAND_ACCENT_INDEX
#define PURR_BRAND_ACCENT_INDEX 5           /* the shell's default accent: index in the Windows Phone palette (5 = Cobalt) */
#endif

#endif /* PURR_BRAND_H */
