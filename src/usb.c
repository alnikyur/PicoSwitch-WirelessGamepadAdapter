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

void
usb_core_task()
{
	tusb_init();

	XInputIdxReport r;
	empty_xinput_report(&r);

	const absolute_time_t boot = get_absolute_time();
	absolute_time_t next_report = boot;

	while (1) {
		// IMPORTANT: tud_task() must run as often as possible, with no
		// sleeps in this loop. All USB events are handled here.
		tud_task();

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