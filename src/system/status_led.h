/**
 * @file status_led.h
 * @brief On-board WS2812 status LED.
 *
 * Priority (first match wins):
 *   button held 10 s+   magenta, steady   release -> heating settings reset
 *   button held 5..10 s cyan, steady      release -> unbind
 *   any fault           red, 1 Hz blink   sensor / boiler / OpenTherm link
 *   waiting for pairing blue, fast blink
 *   no cloud            yellow, short flash every 2 s (heating runs locally)
 *   online, heating     orange, steady
 *   online              green, steady
 */

#ifndef STATUS_LED_H_
#define STATUS_LED_H_

void status_led_init(void);

/* CLI: shows red, green, blue for 1 s each (checks the colour order). */
void status_led_test(void);

#endif /* STATUS_LED_H_ */
