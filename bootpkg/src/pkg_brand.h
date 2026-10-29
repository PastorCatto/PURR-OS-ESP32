/* The names and colours of this flavor of the boot screens.
 *
 * Every value has a default (the stock PURR OS look), and a flavor overrides any of them from a header that
 * `purr brand build` writes from brand.toml: compile with -DPURR_BRAND_HEADER='"path/to/purr_brand.h"'.
 * Colours are RGB565. Strings must be plain ASCII that fits the 8x8 font.
 */
#ifndef PKG_BRAND_H
#define PKG_BRAND_H

#ifdef PURR_BRAND_HEADER
#include PURR_BRAND_HEADER
#endif

#ifndef PKG_BRAND_PRODUCT
#define PKG_BRAND_PRODUCT "PURR OS"             /* the full system, in the menu and the messages */
#endif
#ifndef PKG_BRAND_RECOVERY
#define PKG_BRAND_RECOVERY "KittenOS"           /* the recovery system */
#endif
#ifndef PKG_BRAND_LOGO
#define PKG_BRAND_LOGO "PURR OS!"               /* the wordmark, drawn bold italic */
#endif
#ifndef PKG_BRAND_LOGO_ACCENT_FROM
#define PKG_BRAND_LOGO_ACCENT_FROM 7            /* characters from this position on use the accent colour; a big number for none */
#endif
#ifndef PKG_BRAND_COLOUR_ACCENT
#define PKG_BRAND_COLOUR_ACCENT 0xFDA0          /* the logo accent, the selection border, the default tag */
#endif
#ifndef PKG_BRAND_COLOUR_TEXT
#define PKG_BRAND_COLOUR_TEXT 0xFFFF
#endif
#ifndef PKG_BRAND_COLOUR_DIM
#define PKG_BRAND_COLOUR_DIM 0x94B2
#endif
#ifndef PKG_BRAND_COLOUR_OK
#define PKG_BRAND_COLOUR_OK 0x564F
#endif

#endif /* PKG_BRAND_H */
