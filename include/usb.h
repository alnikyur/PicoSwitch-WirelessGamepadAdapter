#ifndef _USB_H_
#define _USB_H_

#include <stdbool.h>

void usb_core_task(void);

// Request the USB device to detach from (or re-attach to) the host bus.
//
// IMPORTANT: TinyUSB calls are not thread-safe and must only be made from the
// core that runs tud_task() (core0, usb_core_task). These functions only set a
// request flag that usb_core_task() picks up on its next iteration, so they are
// safe to call from the Bluepad32 core (core1) / BTstack timer callbacks.
//
// Detaching makes the emulated Xbox 360 controller disappear from the PC. When
// the PC is turned off or goes to sleep we detach so the host really loses the
// gamepad (instead of the Pico silently keeping sending reports forever).
void usb_request_attach(bool attached);

// Current requested state (true = attached). Useful for logging.
bool usb_is_attach_requested(void);

#endif