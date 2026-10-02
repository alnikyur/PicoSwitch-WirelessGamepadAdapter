#include "report.h"

#include <stdbool.h>
#include <memory.h>

#include <pico/multicore.h>
#include <pico/async_context.h>
#include <pico/cyw43_arch.h>

// used between threads
XInputIdxReport shared_report;

void set_global_gamepad_report(XInputIdxReport *src) {
    if (!src) {
        return;
    }

    async_context_t *context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(context);
    memcpy(&shared_report, src, sizeof(shared_report));
    async_context_release_lock(context);
    multicore_fifo_push_timeout_us(0, 1);
}

uint32_t unused;
void get_global_gamepad_report(XInputIdxReport *dest) {
    multicore_fifo_pop_timeout_us(1, &unused);
    async_context_t *context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(context);
    memcpy(dest, &shared_report, sizeof(*dest));
    async_context_release_lock(context);
}

void empty_xinput_report(XInputIdxReport *rpt) {
    memset(rpt, 0, sizeof(*rpt));
    rpt->report.header0 = XINPUT_REPORT_HEADER_0;
    rpt->report.header1 = XINPUT_REPORT_HEADER_1;
}
