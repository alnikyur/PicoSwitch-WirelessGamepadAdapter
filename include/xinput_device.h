/*
 * Public interface for the emulated Xbox 360 (XInput) controller.
 *
 * The whole Nintendo Switch Pro Controller emulation (handshake, subcommands,
 * calibration, SPI flash emulation) has been removed together with
 * include/SwitchDescriptors.h.
 */

#ifndef _XINPUT_DEVICE_H_
#define _XINPUT_DEVICE_H_

#include <stdbool.h>

#include "XInputDescriptors.h"

// Send one 20-byte XInput report on endpoint IN 0x81.
// Returns false if the device is not ready, not enumerated yet, or the
// endpoint is still busy with a previous transfer.
bool xinput_send_report(XInputReport const *report);

// Last LED / rumble values received from the host on endpoint OUT 0x01.
extern volatile uint8_t xinput_led_state;
extern volatile uint8_t xinput_rumble_l;
extern volatile uint8_t xinput_rumble_r;

#endif
