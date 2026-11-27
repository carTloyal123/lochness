/*
 * Bolt position from two Hall switches, one at each end of the bolt's travel
 * (bench: A3144, dev kit: DRV5032FA). Each pulls its GPIO low while the bolt's
 * magnet is at it; the GPIO's internal pull-up holds it high otherwise.
 * CONFIG_LOCHNESS_BOLT_LOCKED_GPIO / _UNLOCKED_GPIO; -1 for none.
 */
#pragma once

#include <stdbool.h>

#include "lock_core/lc_lock.h"

typedef enum {
    BOLT_SENSE_NONE,     /* no switches configured */
    BOLT_SENSE_LOCKED,   /* magnet at the locked switch */
    BOLT_SENSE_UNLOCKED, /* magnet at the unlocked switch */
    BOLT_SENSE_BETWEEN,  /* at neither: moving, or stopped part way */
    BOLT_SENSE_FAULT,    /* at both: wiring or magnet problem */
} bolt_sense_t;

/* Called (from the FreeRTOS timer task) after each debounced change. */
typedef void (*bolt_sense_cb_t)(bolt_sense_t pos);

/* Configures the GPIOs. Reads work from here on; changes are reported only
 * after bolt_sense_start(). */
void bolt_sense_init(void);
void bolt_sense_start(bolt_sense_cb_t on_change);
bolt_sense_t bolt_sense_read(void);
/* Raw GPIO levels (0 = magnet there). False if no switches are configured. */
bool bolt_sense_levels(int *locked_level, int *unlocked_level);

/* LOCKED / UNLOCKED for a resting position, UNKNOWN otherwise. */
lc_bolt_t bolt_sense_to_lc(bolt_sense_t pos);
const char *bolt_sense_name(bolt_sense_t pos);
