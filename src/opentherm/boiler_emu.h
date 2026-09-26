/**
 * @file boiler_emu.h
 * @brief Emulated 18 kW electric boiler + house on the OpenTherm loopback.
 *
 * For bench testing without an adapter: the emulator answers the master over
 * the same GPIO pads and feeds the modelled room temperature into sensor
 * slot 0. Never start it with an adapter connected to those pads.
 */

#ifndef BOILER_EMU_H_
#define BOILER_EMU_H_

#include <stdbool.h>
#include <stdint.h>

int  boiler_emu_start(void);
void boiler_emu_stop(void);
bool boiler_emu_running(void);

void boiler_emu_set_speed(uint32_t factor); /* model time acceleration */
void boiler_emu_set_fault(uint8_t oem_code); /* 0 clears */
void boiler_emu_set_mute(bool mute);         /* stop answering (comm loss) */
void boiler_emu_set_outside(int16_t x10);
void boiler_emu_set_room(int16_t x10);
void boiler_emu_print(void);

#endif /* BOILER_EMU_H_ */
