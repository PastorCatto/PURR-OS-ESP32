/* purr_battmodel.c - see purr_battmodel.h. */
#include "purr_battmodel.h"

#include <stddef.h>

void purr_battmodel_init(purr_battmodel_t *m)
{
    m->have = 0;
    m->pct = 0;
    m->mv = 0;
    m->ext_hold = 0;
}

void purr_battmodel_feed(purr_battmodel_t *m, int present, int external, int pct, int mv)
{
    if (!present) {
        purr_battmodel_init(m);
        return;
    }
    if (external) {                                    /* USB or the charger holds the line up: nearly full, and the number says nothing finer */
        m->ext_hold = PURR_BATT_EXT_HOLD;
        m->pct = 100;
        m->mv = mv;
        m->have = 1;
        return;
    }
    if (m->ext_hold > 0 && m->have) {                  /* a dip while the reading bounces: keep the picture */
        m->ext_hold--;
        return;
    }
    m->ext_hold = 0;
    if (!m->have) {
        m->pct = pct;
        m->mv = mv;
    } else {
        m->pct = (m->pct * 3 + pct + 2) / 4;           /* a slow follower: the load on the cell moves single readings by several percent */
        m->mv = (m->mv * 3 + mv + 2) / 4;
    }
    m->have = 1;
}

int purr_battmodel_get(const purr_battmodel_t *m, int *pct, int *mv)
{
    if (!m->have) {
        return 0;
    }
    if (pct != NULL) {
        *pct = m->pct;
    }
    if (mv != NULL) {
        *mv = m->mv;
    }
    return 1;
}

int purr_battmodel_charging(const purr_battmodel_t *m)
{
    return m->have && m->ext_hold > 0;
}
