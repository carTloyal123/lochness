/*
 * The lock task owns the lc_t instance; lock_core is not thread-safe, so every
 * other task (console, BLE, drivers) talks to it through this API.
 */
#pragma once

#include "lock_core/lc_lock.h"

typedef int (*lock_task_fn_t)(lc_t *lc, void *arg);

void lock_task_start(void);

/* Run fn(lc, arg) on the lock task and return its result. Blocks the caller. */
int lock_task_call(lock_task_fn_t fn, void *arg);

/* Driver events. Safe from tasks and timer callbacks (not ISRs). */
void lock_task_post_motor_done(bool success);
void lock_task_post_door(lc_door_t door);
void lock_task_post_bolt_position(lc_bolt_t bolt);

/* Persist the current config (call from inside lock_task_call). */
void lock_task_save_config(const lc_t *lc);
