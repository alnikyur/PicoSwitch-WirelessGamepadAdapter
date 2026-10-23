"""Stress the real report copy code with two host threads and a mock SDK lock."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
lock_header = r'''
#include <pthread.h>
typedef pthread_mutex_t critical_section_t;
static void critical_section_init(critical_section_t *s) { pthread_mutex_init(s, 0); }
static void critical_section_enter_blocking(critical_section_t *s) { pthread_mutex_lock(s); }
static void critical_section_exit(critical_section_t *s) { pthread_mutex_unlock(s); }
'''
test = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "report.h"
static void *writer(void *unused) {
    XInputIdxReport r;
    for (int i = 0; i < 200000; i++) {
        memset(&r, (i & 1) ? 0x55 : 0xaa, sizeof(r));
        set_global_gamepad_report(&r);
    }
    return 0;
}
int main(void) {
    report_init();
    XInputIdxReport r;
    get_global_gamepad_report(&r);
    assert(r.report.header0 == XINPUT_REPORT_HEADER_0);
    assert(r.report.header1 == XINPUT_REPORT_HEADER_1);
    memset(&r, 0x55, sizeof(r));
    set_global_gamepad_report(&r);
    pthread_t thread;
    assert(pthread_create(&thread, 0, writer, 0) == 0);
    for (int i = 0; i < 200000; i++) {
        get_global_gamepad_report(&r);
        unsigned char *bytes = (unsigned char *)&r;
        assert(bytes[0] == 0x55 || bytes[0] == 0xaa);
        for (unsigned j = 1; j < sizeof(r); j++) assert(bytes[j] == bytes[0]);
    }
    pthread_join(thread, 0);
    puts("Concurrent report handoff passed");
}
'''
with tempfile.TemporaryDirectory(prefix="report-test-") as temp:
    path = Path(temp)
    (path / "pico").mkdir()
    (path / "pico/critical_section.h").write_text(lock_header)
    (path / "test.c").write_text(test)
    exe = path / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-pthread", "-I", str(path),
                    "-I", str(root / "include"), str(root / "src/report.c"),
                    str(path / "test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
