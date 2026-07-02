#ifndef GOPRO_CONTROL_H
#define GOPRO_CONTROL_H

/* High-level actions. The physical buttons (buttons.c) call into
 * these rather than talking to gopro_ble.c directly, so there's
 * exactly one place that owns the "what does a toggle/cycle mean"
 * logic. */

void gopro_control_toggle_shutter(void);
void gopro_control_power_button(void);
void gopro_control_cycle_mode(void);

/** Schedules a clean device restart (esp_restart) shortly after
 *  returning, so callers (e.g. an HTTP handler) have time to send
 *  their response first. */
void gopro_control_reset_device(void);

#endif // GOPRO_CONTROL_H
