#include "report.h"

#include <stdbool.h>
#include <memory.h>

#include <pico/critical_section.h>

// used between threads
static XInputIdxReport shared_report;
static critical_section_t report_lock;

void report_init(void) {
    critical_section_init(&report_lock);
    empty_xinput_report(&shared_report);
}

void set_global_gamepad_report(XInputIdxReport *src) {
    if (!src) {
        return;
    }

    critical_section_enter_blocking(&report_lock);
    memcpy(&shared_report, src, sizeof(shared_report));
    critical_section_exit(&report_lock);
}

void get_global_gamepad_report(XInputIdxReport *dest) {
    critical_section_enter_blocking(&report_lock);
    memcpy(dest, &shared_report, sizeof(*dest));
    critical_section_exit(&report_lock);
}

void empty_xinput_report(XInputIdxReport *rpt) {
    memset(rpt, 0, sizeof(*rpt));
    rpt->report.header0 = XINPUT_REPORT_HEADER_0;
    rpt->report.header1 = XINPUT_REPORT_HEADER_1;
}
