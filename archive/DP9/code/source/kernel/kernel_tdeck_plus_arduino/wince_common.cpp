// wince_common.cpp — shared WinCE chrome helpers.
#include "wince_common.h"

void wince_draw_raised(const mw_gl_draw_info_t *d,
                        int16_t x, int16_t y, int16_t w, int16_t h,
                        uint32_t fill)
{
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(fill); mw_gl_rectangle(d, x, y, w, h);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_hline(d, x, x+w-1, y); mw_gl_vline(d, x, y, y+h-1);
    mw_gl_set_fg_colour(WCE_SHD);
    mw_gl_hline(d, x+1, x+w-2, y+h-2); mw_gl_vline(d, x+w-2, y+1, y+h-2);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_hline(d, x, x+w-1, y+h-1); mw_gl_vline(d, x+w-1, y, y+h-1);
}

void wince_draw_sunken(const mw_gl_draw_info_t *d,
                        int16_t x, int16_t y, int16_t w, int16_t h,
                        uint32_t fill)
{
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(fill); mw_gl_rectangle(d, x, y, w, h);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_hline(d, x, x+w-1, y); mw_gl_vline(d, x, y, y+h-1);
    mw_gl_set_fg_colour(WCE_SHD);
    mw_gl_hline(d, x+1, x+w-2, y+1); mw_gl_vline(d, x+1, y+1, y+h-2);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_hline(d, x, x+w-1, y+h-1); mw_gl_vline(d, x+w-1, y, y+h-1);
}
