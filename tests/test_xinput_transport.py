"""Compile the real XInput driver against a small fake TinyUSB host lifecycle."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_DEBUG 0
#define CONTROL_STAGE_SETUP 0
#define CONTROL_STAGE_DATA 1
#define CONTROL_STAGE_ACK 2
#define TUSB_DESC_ENDPOINT 5
#define TUSB_XFER_INTERRUPT 3
#define XFER_RESULT_SUCCESS 0
typedef int xfer_result_t;
typedef struct __attribute__((packed)) {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting,
            bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} tusb_desc_interface_t;
typedef struct __attribute__((packed)) {
    uint8_t bLength, bDescriptorType, bEndpointAddress;
    struct { uint8_t xfer:2; uint8_t rest:6; } bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} tusb_desc_endpoint_t;
typedef struct {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} tusb_control_request_t;
typedef struct {
    void (*init)(void);
    void (*reset)(uint8_t);
    uint16_t (*open)(uint8_t, tusb_desc_interface_t const*, uint16_t);
    bool (*control_xfer_cb)(uint8_t, uint8_t, tusb_control_request_t const*);
    bool (*xfer_cb)(uint8_t, uint8_t, xfer_result_t, uint32_t);
    void (*sof)(uint8_t, uint32_t);
} usbd_class_driver_t;
static bool ready, busy[256], claimed[256], fail_out;
static unsigned out_submits, control_submits;
static uint16_t control_length;
static uint8_t *control_buffer;
static bool tud_ready(void) { return ready; }
static bool usbd_edpt_busy(uint8_t port, uint8_t ep) { return busy[ep]; }
static bool usbd_edpt_claim(uint8_t port, uint8_t ep) {
    if (claimed[ep] || busy[ep]) return false;
    return claimed[ep] = true;
}
static void usbd_edpt_release(uint8_t port, uint8_t ep) { claimed[ep] = false; }
static bool usbd_edpt_open(uint8_t port, tusb_desc_endpoint_t const *ep) { return true; }
static bool usbd_edpt_xfer(uint8_t port, uint8_t ep, uint8_t *data, uint16_t len) {
    if (ep == 1 && fail_out) return false;
    busy[ep] = true;
    if (ep == 1) out_submits++;
    return true;
}
static bool tud_control_xfer(uint8_t port, tusb_control_request_t const *req, void *data, uint16_t len) {
    control_submits++; control_length = len; control_buffer = data; return true;
}
'''
test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xinput_device.c"
static void complete_out(int result) {
    busy[1] = claimed[1] = false;
    xinput_xfer_cb(0, 1, result, 0);
}
int main(void) {
    uint8_t drivers;
    const usbd_class_driver_t *driver = usbd_app_driver_get_cb(&drivers);
    assert(drivers == 1);
    driver->init();
    // Real SET_CONFIGURATION ordering: open() precedes ready=true.
    ready = false;
    const tusb_desc_interface_t *itf = (const void *)(xinput_configuration_descriptor + 9);
    assert(driver->open(0, itf, XINPUT_CONFIG_TOTAL_LEN - 9) == XINPUT_CONFIG_TOTAL_LEN - 9);
    assert(out_submits == 0);
    ready = true;
    XInputReport report = { .header1 = XINPUT_REPORT_HEADER_1 };
    assert(xinput_send_report(&report));
    assert(out_submits == 1); // This assertion fails with the original driver.
    // A busy IN endpoint must not block recovery of OUT reception.
    busy[1] = claimed[1] = false;
    assert(!xinput_send_report(&report));
    assert(out_submits == 2);
    xinput_send_report(&report);
    assert(out_submits == 2); // No duplicate receive while OUT is busy.
    // Completion during suspend cannot arm; resume must recover without a callback.
    ready = false; complete_out(0);
    assert(!busy[1]);
    ready = true; xinput_send_report(&report);
    assert(busy[1] && out_submits == 3);
    // A failed submission releases the claim so the next iteration can retry.
    fail_out = true; complete_out(1);
    assert(!claimed[1]);
    fail_out = false; xinput_send_report(&report);
    assert(out_submits == 4);
    // Host reset/reconfiguration has the same ordering as initial setup.
    driver->reset(0); memset(busy, 0, sizeof(busy)); memset(claimed, 0, sizeof(claimed));
    ready = false; driver->open(0, itf, XINPUT_CONFIG_TOTAL_LEN - 9);
    ready = true; xinput_send_report(&report);
    assert(out_submits == 5);
    tusb_control_request_t req = {0xC1, 1, 0x100, 0, 20};
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    assert(control_submits == 1 && control_length == 20);
    assert(control_buffer[0] == 0 && control_buffer[1] == 20);
    for (int i = 2; i < 20; i++) assert(control_buffer[i] == 0);
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    assert(control_submits == 1);
    req.wIndex = 1;
    assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    assert(!driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    puts("XInput configure/reset/resume/OUT/control scenarios passed");
}
'''
with tempfile.TemporaryDirectory(prefix="xinput-test-") as temp:
    path = Path(temp)
    (path / "device").mkdir()
    (path / "tusb.h").write_text(header)
    (path / "device/usbd_pvt.h").write_text('#include "tusb.h"\n')
    (path / "test.c").write_text(test)
    exe = path / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-I", str(path),
                    "-I", str(root / "include"), "-I", str(root / "src"),
                    str(path / "test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
