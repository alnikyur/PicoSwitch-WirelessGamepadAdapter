#include "usb.h"

#include <tusb.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pico/stdlib.h>
#include "uni_log.h"

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
// USB stays attached across PC sleep and Bluetooth disconnects.
// Only the host controls configuration/reset; no timed software re-enumeration.
static bool usb_remote_wakeup_allowed;

void tud_mount_cb(void) {
	logi("USB: mounted\n");
}
void tud_umount_cb(void) {
	usb_remote_wakeup_allowed = false;
	logi("USB: unmounted\n");
}
void tud_suspend_cb(bool remote_wakeup_en) {
	usb_remote_wakeup_allowed = remote_wakeup_en;
	logi("USB: suspended, remote_wakeup=%d\n", remote_wakeup_en);
}
void tud_resume_cb(void) {
	usb_remote_wakeup_allowed = false;
	logi("USB: resumed\n");
}

static bool usb_handle_suspend(bool input_changed, bool input_active)
{
	if (!tud_suspended())
		return false;
	if (input_changed && input_active && usb_remote_wakeup_allowed)
		tud_remote_wakeup();
	return true;
}

void
usb_core_task()
{
	tusb_init();
	tud_connect();

	XInputIdxReport r;
	empty_xinput_report(&r);
	XInputReport previous_report = r.report;
	const XInputReport neutral_report = r.report;

	const absolute_time_t boot = get_absolute_time();
	absolute_time_t next_report = boot;
	uint64_t next_health_log_us = time_us_64() + 60000000;

	while (1) {
		// IMPORTANT: tud_task() must run as often as possible, with no
		// sleeps in this loop. All USB events are handled here.
		tud_task();

		uint64_t now = time_us_64();
		if (now >= next_health_log_us) {
			next_health_log_us = now + 60000000;
			logi("USB: alive mounted=%d suspended=%d\n",
			     tud_mounted(), tud_suspended());
		}

		// Send reports on a fixed period, without blocking.
		if (!time_reached(next_report)) {
			continue;
		}
		next_report = make_timeout_time_ms(USB_REPORT_PERIOD_MS);

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
		bool changed = memcmp(&previous_report, &r.report, sizeof(r.report)) != 0;
		previous_report = r.report;
		// tud_ready() is false while suspended: handle wake before checking it.
		// Only new input may request wake, and only when the host permitted it.
		if (usb_handle_suspend(changed,
		        memcmp(&r.report, &neutral_report, sizeof(r.report)) != 0)) {
			continue;
		}
		if (!tud_ready())
			continue;

		// xinput_send_report() checks tud_ready() and endpoint busy
		// state itself, so no extra guarding is needed here.
		xinput_send_report(&r.report);
	}
}
