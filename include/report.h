#ifndef _REPORT_H_
#define _REPORT_H_

#include <stdint.h>

#include "xinput_device.h"

// Shared report passed from the Bluepad32 core (core1) to the USB core (core0).
// The index is kept so a future version can report which "seat" the sample
// came from; the current XInput device sends a single merged controller.
typedef struct {
	uint8_t idx;
	XInputReport report;
} XInputIdxReport;

void set_global_gamepad_report(XInputIdxReport *rpt);
void get_global_gamepad_report(XInputIdxReport *rpt);

// Fill a report with neutral values (sticks centered, no buttons).
void empty_xinput_report(XInputIdxReport *rpt);

#endif
