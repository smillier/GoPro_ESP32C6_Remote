#ifndef LED_STATUS_H
#define LED_STATUS_H

/** Initializes the onboard WS2812 (GPIO8 on the ESP32-C6 Super Mini)
 *  and starts the background task that drives it, reflecting the
 *  current app_state (connection state + recording flag). Call once
 *  from app_main(), after app_state_init(). */
void led_status_init(void);

#endif // LED_STATUS_H
