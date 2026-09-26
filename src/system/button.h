/**
 * @file button.h
 * @brief Service button (BOOT, GPIO0): actions on RELEASE, by hold time.
 *
 *   5..10 s -> unbind: Wi-Fi + pairing are reset, heating keeps running
 *   >= 10 s -> heating settings back to defaults
 *
 * Nothing happens while the button is held, so the chip never reboots with
 * GPIO0 (a strapping pin) low and cannot end up in download mode.
 */

#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdint.h>

typedef enum {
    BUTTON_HOLD_NONE    = 0,
    BUTTON_HOLD_UNBIND  = 1, /* released now -> unbind */
    BUTTON_HOLD_FACTORY = 2, /* released now -> settings reset */
} button_hold_e;

void          button_init(void);
button_hold_e button_hold_level(void); /* for the LED while held */

#endif /* BUTTON_H_ */
