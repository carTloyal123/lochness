/*
 * The bolt motor, behind lock_core's motor hooks. One of two drivers is built
 * (menuconfig: Lochness lock unit, then Motor driver):
 *
 *   motor_hbridge.c  a real motor on an H-bridge: TB6612FNG (bench) or DRV8833
 *                    (dev kit), driven until the bolt switch for the target
 *                    position reports (bolt_sense.h), then braked.
 *   motor_sim.c      no motor: reports success after a fixed delay (QEMU, or a
 *                    bare board).
 *
 * Either reports the end of a move with lock_task_post_motor_done(). A move
 * that never reaches its switch is stopped by lock_core's motor timeout
 * (config motor-timeout) and reported as jammed.
 */
#pragma once

#include <stdbool.h>

#include "bolt_sense.h"
#include "lock_core/lc_lock.h"

void motor_init(void);
/* lock_core hooks: run on the lock task. */
void motor_start(void *ctx, lc_op_t op);
void motor_stop(void *ctx);
/* Feed every bolt switch change here (any task). */
void motor_on_bolt(bolt_sense_t pos);
/* Simulator only: make the next move report a jam. Returns false if the
 * driver can't (the real motor jams for real). */
bool motor_fail_next(void);
/* Name for logs and `status`. */
const char *motor_driver_name(void);
