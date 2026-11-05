/*
 * purr_battmodel.h - what the OS makes of the battery gauge's readings (the rules, free of hardware, so a host test covers them).
 *
 * The gauge (battery-adc) reads the cell line through a divider. On battery that is the cell: the model smooths it (the voltage sags under load and wobbles).
 * On USB power the charger lifts the line to 4.5 to 5 V and it bounces, so a reading flagged "external" means charging and nearly full: the level is held at 100
 * and not smoothed in, and a few dips back to a cell-like voltage are ignored so the charging picture does not flicker.
 */
#ifndef PURR_BATTMODEL_H
#define PURR_BATTMODEL_H

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_BATT_EXT_HOLD 2            /* readings that may dip while plugged in before it counts as unplugged (readings come every 20 s) */

typedef struct {
    int have, pct, mv;
    int ext_hold;                       /* readings left in which a dip is still "plugged in" */
} purr_battmodel_t;

void purr_battmodel_init(purr_battmodel_t *m);

/* One reading from the gauge: present (a cell is there), external (the line is held above a resting cell's), the gauge's percent and millivolts. */
void purr_battmodel_feed(purr_battmodel_t *m, int present, int external, int pct, int mv);

/* 1 and the level if there is a reading; 0 if there is no battery. */
int purr_battmodel_get(const purr_battmodel_t *m, int *pct, int *mv);

/* 1 while plugged in (an external reading, or a dip within PURR_BATT_EXT_HOLD readings of one). */
int purr_battmodel_charging(const purr_battmodel_t *m);

#ifdef __cplusplus
}
#endif

#endif
