#ifndef BUTTONS_H
#define BUTTONS_H

/**
 * Configures GPIO2 (shutter start/stop), GPIO3 (power/sleep) and
 * GPIO4 (mode: Video/Photo/Timelapse) as debounced inputs and starts
 * the task that reacts to presses. Call once from app_main().
 *
 * Wiring: each button goes between its GPIO and GND. Internal
 * pull-ups are enabled in software, so no external resistors needed.
 */
void buttons_init(void);

#endif // BUTTONS_H
