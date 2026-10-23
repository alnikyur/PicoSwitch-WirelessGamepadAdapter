"""Exercise the actual USB link state machine with fake TinyUSB and time."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/usb.c").read_text(encoding="utf-8")
code = source[source.index("#define USB_REPORT_PERIOD_MS"):source.index("void\nusb_core_task")]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define logi(...) ((void)0)
typedef int critical_section_t;
static void critical_section_init(critical_section_t *s) { *s = 0; }
static void critical_section_enter_blocking(critical_section_t *s) { assert(!*s); *s = 1; }
static void critical_section_exit(critical_section_t *s) { assert(*s); *s = 0; }
static bool mounted, suspended;
static unsigned connects, disconnects, wakes;
static bool tud_mounted(void) { return mounted; }
static bool tud_suspended(void) { return suspended; }
static void tud_connect(void) { connects++; }
// Match this SDK: disconnect does NOT clear TinyUSB's cached mounted state.
static void tud_disconnect(void) { disconnects++; }
static void tud_remote_wakeup(void) { wakes++; }
'''
cases = r'''
int main(void) {
    // Configuration and resume never request a software USB disconnect/connect.
    mounted = true; tud_mount_cb();
    for (int cycle = 0; cycle < 100; cycle++) {
        suspended = true; tud_suspend_cb(true);
        assert(usb_handle_suspend(false, false));
        assert(usb_handle_suspend(true, false)); // Clearing buttons must not wake PC.
        assert(wakes == (unsigned)cycle);
        assert(usb_handle_suspend(true, true));
        assert(wakes == (unsigned)cycle + 1);
        suspended = false; tud_resume_cb();
        assert(!usb_handle_suspend(true, true));
        assert(connects == 0 && disconnects == 0);
    }
    suspended = true; tud_suspend_cb(false);
    assert(usb_handle_suspend(true, true) && wakes == 100);
    // Host resets/unmounts are allowed, but never cause firmware re-enumeration.
    mounted = false; tud_umount_cb();
    mounted = true; tud_mount_cb();
    assert(connects == 0 && disconnects == 0);
    puts("Persistent USB suspend/resume scenarios passed");
}
'''
# Guard the actual loop too: no hidden reconnect timer or GPIO/BT attach request.
assert "tud_disconnect(" not in source
assert source.count("tud_connect(") == 1  # Once at firmware startup only.
platform = (root / "src/pico_switch_platform.c").read_text(encoding="utf-8")
assert "usb_request_attach(" not in platform
assert "usb_request_reenumeration(" not in platform
with tempfile.TemporaryDirectory(prefix="usb-recovery-test-") as temp:
    path = Path(temp)
    unit = path / "test.c"
    unit.write_text(stubs + code + cases, encoding="utf-8")
    exe = path / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", str(unit), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
