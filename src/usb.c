#include "usb.h"

#include <tusb.h>
#include <stdint.h>
#include <stdbool.h>

#include <pico/stdlib.h>
#include <pico/cyw43_arch.h>
#include <pico/multicore.h>
#include <pico/async_context.h>

#include "report.h"
#include "xinput_device.h"

// Set to 1 (or build with -DUSB_SELFTEST=1) to bypass bluepad32 completely:
// button A is toggled once per second. If A blinks on the host, the USB path
// works and the problem is upstream (bluepad32 -> shared report).
#ifndef USB_SELFTEST
#define USB_SELFTEST 0
#endif

// Time (us) after boot during which empty reports are sent, so the host
// always sees a live device even before a gamepad connects.
#define USB_WARMUP_US (5u * 1000u * 1000u)

// Report period. Matches bInterval = 4 ms in the IN endpoint descriptor.
#define USB_REPORT_PERIOD_MS 4

// ---------------------------------------------------------------------------
// Attach / detach request shared between core1 (Bluepad32) and core0 (USB).
// ---------------------------------------------------------------------------
// Set from core1 when the PC turns off / goes to sleep, so the emulated Xbox
// 360 controller actually disappears from the host. TinyUSB can only be driven
// from core0, so core1 just posts a request here.
static volatile bool usb_attach_requested = true;

void usb_request_attach(bool attached)
{
	usb_attach_requested = attached;
}

bool usb_is_attach_requested(void)
{
	return usb_attach_requested;
}

void
usb_core_task()
{
	tusb_init();

	XInputIdxReport r;
	empty_xinput_report(&r);

	const absolute_time_t boot = get_absolute_time();
	absolute_time_t next_report = boot;
	// Start from "not attached" and issue an explicit tud_connect() on the
	// first iteration. This guarantees a known initial state on the bus
	// regardless of what tusb_init() left configured.
	bool usb_attached = false;

	while (1) {
		// IMPORTANT: tud_task() must run as often as possible, with no
		// sleeps in this loop. All USB events are handled here.
		tud_task();

		// Apply a pending attach/detach request. This must run on this core
		// because it touches the TinyUSB device stack directly.
		bool want_attached = usb_attach_requested;
		if (want_attached != usb_attached) {
			if (want_attached) {
				tud_connect();
			} else {
				tud_disconnect();
			}
			usb_attached = want_attached;
		}

		// While detached, do not send anything: the host must see the device
		// as gone. tud_ready() would go false anyway, but skipping the report
		// path entirely makes the intent explicit and avoids a race right
		// after tud_disconnect().
		if (!usb_attached) {
			continue;
		}

		// Send reports on a fixed period, without blocking.
		if (!time_reached(next_report)) {
			continue;
		}
		next_report = make_timeout_time_ms(USB_REPORT_PERIOD_MS);

		if (!tud_ready()) {
			continue;
		}

		if (tud_suspended()) {
			tud_remote_wakeup();
			continue;
		}

		int64_t elapsed_us = absolute_time_diff_us(boot, get_absolute_time());

#if USB_SELFTEST
		empty_xinput_report(&r);
		if ((elapsed_us / 1000000) & 1) {
			r.report.buttons3 |= XINPUT_BUTTON_A;
		}
#else
		if (elapsed_us < (int64_t) USB_WARMUP_US) {
			empty_xinput_report(&r);
		} else {
			get_global_gamepad_report(&r);
			// shared_report is zero-initialised until bluepad32 writes
			// to it for the first time: always force a valid header.
			r.report.header0 = XINPUT_REPORT_HEADER_0;
			r.report.header1 = XINPUT_REPORT_HEADER_1;
		}
#endif

		// xinput_send_report() checks tud_ready() and endpoint busy
		// state itself, so no extra guarding is needed here.
		xinput_send_report(&r.report);
	}
}