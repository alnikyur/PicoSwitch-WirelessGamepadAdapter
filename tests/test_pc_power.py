"""Run the actual power-control code with fake GPIO, time and BTstack timers.

Requires Python 3 and a host C compiler (CC, default cc); no Pico hardware needed.
"""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/pico_switch_platform.c").read_text(encoding="utf-8")
power_code = source[source.index("#define POWER_BTN_GPIO"):source.index("// Declarations")]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define logi(...) ((void)0)
#define GPIO_OUT 1
#define GPIO_IN 0
#define CONFIG_BLUEPAD32_MAX_DEVICES 0
typedef struct timer { bool active; unsigned ms; void (*handler)(struct timer*); } btstack_timer_source_t;
typedef void uni_hid_device_t;
static uint64_t now;
static int input, output, assertions, shutdowns, attaches;
static uint64_t time_us_64(void) { return now; }
static int gpio_get(int pin) { return input; }
static void gpio_init(int pin) {}
static void gpio_set_dir(int pin, int dir) {}
static void gpio_pull_up(int pin) {}
static void gpio_pull_down(int pin) {}
static void gpio_put(int pin, int value) { output = value; assertions += value; }
static void btstack_run_loop_set_timer_handler(btstack_timer_source_t *t, void (*fn)(btstack_timer_source_t*)) { t->handler = fn; }
static void btstack_run_loop_set_timer(btstack_timer_source_t *t, unsigned ms) { t->ms = ms; }
static void btstack_run_loop_add_timer(btstack_timer_source_t *t) { assert(!t->active); t->active = true; }
static void btstack_run_loop_remove_timer(btstack_timer_source_t *t) { t->active = false; }
static uni_hid_device_t* uni_hid_device_get_instance_for_idx(int i) { return 0; }
static bool uni_hid_device_is_gamepad(uni_hid_device_t *d) { return false; }
static void uni_hid_device_disconnect(uni_hid_device_t *d) {}
static void usb_request_attach(bool attach) { if (attach) attaches++; else shutdowns++; }
'''
cases = r'''
static void tick(unsigned ms) {
    now = (uint64_t)ms * 1000;
    pc_status_timer.active = false;
    pc_status_timer_timeout(&pc_status_timer);
}
static void reset(int level) {
    now = 0; input = level; assertions = output = shutdowns = attaches = 0;
    btn_timer_active = deferred_disconnect_timer_active = false;
    btn_off_timer.active = pc_status_timer.active = deferred_disconnect_timer.active = false;
    last_pc_state = false;
    pc_power_control_init();
}
int main(void) {
    // A connection at boot must wait for confirmed LOW, then press only once.
    reset(0); power_request_pending = true;
    tick(0); tick(99); assert(assertions == 0 && power_request_pending);
    tick(100); assert(!power_request_pending && !last_pc_state);
    assert(assertions == (PC_POWER_DRY_RUN ? 0 : 1));
    tick(110); assert(assertions == (PC_POWER_DRY_RUN ? 0 : 1));
    if (!PC_POWER_DRY_RUN) {
        assert(btn_off_timer.ms == 300 && output == 1);
        assert(!press_power_button_safe(true)); // No duplicate timer registration.
        btn_off_timer.active = false;
        btn_off_timer.handler(&btn_off_timer);
        assert(output == 0 && !btn_timer_active);
    }
    // HIGH at boot, and reconnecting to an ON PC, must never auto-press.
    reset(1); power_request_pending = true;
    tick(100); tick(2999); assert(power_request_pending && assertions == 0);
    tick(3000); assert(last_pc_state && !power_request_pending && assertions == 0);
    power_request_pending = true; tick(3010); assert(assertions == 0);
    // A short LOW glitch is neither shutdown nor permission to press.
    input = 0; tick(3020); assert(!press_power_button_safe(false));
    input = 1; tick(3070); assert(last_pc_state && !deferred_disconnect_timer_active);
    // Stable LOW after ON preserves the existing shutdown notification.
    input = 0; tick(3080); tick(3180);
    assert(!last_pc_state && deferred_disconnect_timer_active);
    deferred_disconnect_timer.active = false;
    deferred_disconnect_timer.handler(&deferred_disconnect_timer);
    assert(shutdowns == 1);
    input = 1; tick(3200); tick(6200);
    assert(last_pc_state && attaches == 1);
    // Sleep blinking: 1 s HIGH / 1 s LOW never qualifies as ON.
    reset(1); power_request_pending = true;
    for (unsigned ms = 0; ms <= 5000; ms += 10) {
        input = (ms / 1000) % 2 == 0;
        tick(ms);
        assert(!last_pc_state);
    }
    assert(assertions == (PC_POWER_DRY_RUN ? 0 : 1));
    // A stale previous OFF state cannot bypass a newly HIGH input.
    reset(0); tick(100); input = 1;
    assert(!press_power_button_safe(false) && assertions == 0);
    // Forced presses still work, but dry-run suppresses even those.
    assert(press_power_button_safe(true));
    assert(assertions == (PC_POWER_DRY_RUN ? 0 : 1));
    puts("PC power scenarios passed");
}
'''
with tempfile.TemporaryDirectory(prefix="pc-power-test-") as temp:
    path = Path(temp)
    unit = path / "test.c"
    unit.write_text(stubs + power_code + cases, encoding="utf-8")
    for dry_run in (0, 1):
        exe = path / f"test-{dry_run}"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", f"-DPC_POWER_DRY_RUN={dry_run}", str(unit), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    for pin, extra in [(23, []), (24, []), (25, []), (29, []), (15, []),
                       (0, ["-DPC_STATE_DEBUG_UART=1"]), (1, ["-DPC_STATE_DEBUG_UART=1"])]:
        unit.write_text(stubs + power_code.replace("#define PC_STATE_PIN 14", f"#define PC_STATE_PIN {pin}") + cases, encoding="utf-8")
        result = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-fsyntax-only", *extra, str(unit)], capture_output=True, text=True)
        assert result.returncode != 0 and "PC_STATE_PIN" in result.stderr, result.stderr
    print("Reserved/conflicting GPIO checks passed")
