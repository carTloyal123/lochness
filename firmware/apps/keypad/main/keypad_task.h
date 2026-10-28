/*
 * The keypad task owns the kp_t state machine; input, BLE and console talk to
 * it through these posts.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "keypad_core/kp.h"

void keypad_task_start(void);

void keypad_task_post_key(char key);
void keypad_task_post_pair(void);
void keypad_task_post_forget(void);

/* From the BLE central. */
void keypad_task_post_connected(const uint8_t *addr, size_t len);
void keypad_task_post_disconnected(void);
void keypad_task_post_frame(const uint8_t *frame, size_t len);

/* Run fn on the keypad task (console use). */
typedef void (*keypad_task_fn_t)(kp_t *kp, void *arg);
void keypad_task_call(keypad_task_fn_t fn, void *arg);
