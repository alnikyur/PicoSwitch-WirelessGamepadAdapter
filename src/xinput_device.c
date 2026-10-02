/*
 * Custom TinyUSB class driver for the emulated wired Xbox 360 controller.
 *
 * TinyUSB has no built-in Xbox 360 class (and we deliberately avoid HID),
 * so this driver is installed through usbd_app_driver_get_cb().
 *
 * It only forwards the standard class-driver callbacks to our own handler.
 * No authentication (0x40/0x41) is implemented: Linux' xpad driver and
 * Steam Input do not need it. Windows xusb22.sys does, and this device will
 * therefore NOT enumerate correctly on Windows.
 *
 * IMPORTANT - API verified against the TinyUSB bundled with this Pico SDK
 * (src/device/usbd_pvt.h and src/device/usbd.h):
 *   usbd_class_driver_t has exactly these members:
 *       (char const* name  - only when CFG_TUSB_DEBUG >= 2)
 *       void     (*init)(void)
 *       void     (*reset)(uint8_t rhport)
 *       uint16_t (*open)(uint8_t rhport, tusb_desc_interface_t const*, uint16_t)
 *       bool     (*control_xfer_cb)(uint8_t, uint8_t, tusb_control_request_t const*)
 *       bool     (*xfer_cb)(uint8_t, uint8_t, xfer_result_t, uint32_t)
 *       void     (*sof)(uint8_t, uint32_t)
 *   There is NO `deinit` member in this version.
 *   Endpoint API:
 *       usbd_edpt_xfer(rhport, ep_addr, buf, len)   <- no `bool is_isr` argument
 *       usbd_edpt_claim / usbd_edpt_release / usbd_edpt_busy /
 *       usbd_edpt_stalled / usbd_edpt_ready
 *   usbd_edpt_release() must be declared by including "device/usbd_pvt.h",
 *   it is private API and is not exported through tusb.h.
 */

#include <tusb.h>

#include "XInputDescriptors.h"

// Private device stack header: needed for usbd_class_driver_t and
// usbd_edpt_release(). This is the same header used by pico_stdio_usb.
#include "device/usbd_pvt.h"

//--------------------------------------------------------------------+
// Local state
//--------------------------------------------------------------------+

static uint8_t xinput_ep_in;
static uint8_t xinput_ep_out;

// Single reusable buffer for the OUT endpoint (LED / rumble packets).
// The real packet is up to 32 bytes; we only inspect the first few bytes.
static CFG_TUSB_MEM_ALIGN uint8_t xinput_out_buf[XINPUT_ENDPOINT_SIZE];

// Last received LED / rumble command. Not applied to the Bluepad32 controller
// in this version, but stored so a later feature can use it.
volatile uint8_t xinput_led_state = 0;
volatile uint8_t xinput_rumble_l = 0;
volatile uint8_t xinput_rumble_r = 0;

//--------------------------------------------------------------------+
// OUT endpoint handling (host -> device)
//--------------------------------------------------------------------+

// Called each time a packet arrives on EP01.
static inline void xinput_handle_out_packet(uint8_t const *buf, uint16_t len)
{
	if (len == 0) {
		return;
	}

	if (buf[0] == 0x00) {
		// "LED" packet: byte 0 = 0x00, byte 1 = pattern (0..14)
		if (len >= 2) {
			xinput_led_state = buf[1];
		}
	} else if (buf[0] == 0x01) {
		// "Rumble" packet: byte 1 = left motor, byte 2 = right motor
		if (len >= 3) {
			xinput_rumble_l = buf[1];
			xinput_rumble_r = buf[2];
		}
	}
	// Any other packet is intentionally ignored. This is what prevents the
	// player-LED / rumble stream from ever blocking the OUT endpoint.
}

// (Re)arm a read on the OUT endpoint. Safe to call when already armed
// because it checks the busy flag first.
static void xinput_arm_out(void)
{
	if (xinput_ep_out == 0 || !tud_ready()) {
		return;
	}
	if (usbd_edpt_busy(0, xinput_ep_out)) {
		return;
	}
	if (!usbd_edpt_claim(0, xinput_ep_out)) {
		return;
	}
	if (!usbd_edpt_xfer(0, xinput_ep_out, xinput_out_buf, sizeof(xinput_out_buf))) {
		usbd_edpt_release(0, xinput_ep_out);
	}
}

//--------------------------------------------------------------------+
// TinyUSB class driver
//--------------------------------------------------------------------+

static void xinput_init(void)
{
	xinput_ep_in = 0;
	xinput_ep_out = 0;
}

static void xinput_reset(uint8_t rhport)
{
	(void) rhport;
	xinput_ep_in = 0;
	xinput_ep_out = 0;
}

static uint16_t xinput_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
	// Only bind to our vendor-specific X360 interface.
	if (itf_desc->bInterfaceClass != 0xFF ||
	    itf_desc->bInterfaceSubClass != 0x5D ||
	    itf_desc->bInterfaceProtocol != 0x01) {
		return 0;
	}

	/* Descriptor layout after the interface descriptor:
	 *   [0] vendor specific descriptor, 17 bytes (bDescriptorType = 0x21)
	 *   [1] OUT endpoint, 7 bytes
	 *   [2] IN  endpoint, 7 bytes
	 */
	uint8_t const *p_desc = (uint8_t const *) itf_desc;
	uint16_t remaining = max_len;
	uint16_t consumed = 0;

	// interface descriptor
	p_desc += itf_desc->bLength;
	remaining -= itf_desc->bLength;
	consumed += itf_desc->bLength;

	// Walk the class-specific descriptor followed by the endpoints.
	while (remaining >= 2) {
		uint8_t dlen = p_desc[0];
		uint8_t dtype = p_desc[1];
		if (dlen == 0) {
			break;
		}

		if (dtype == TUSB_DESC_ENDPOINT && dlen >= 7) {
			tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *) p_desc;
			if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
				if (!usbd_edpt_open(rhport, ep)) {
					return 0;
				}
				if (ep->bEndpointAddress & 0x80) {
					xinput_ep_in = ep->bEndpointAddress;
				} else {
					xinput_ep_out = ep->bEndpointAddress;
				}
			}
		}
		// dtype == 0x21 is the interface-specific vendor descriptor:
		// nothing to open, just skip it.

		p_desc += dlen;
		remaining -= dlen;
		consumed += dlen;
	}

	if (xinput_ep_in == 0 || xinput_ep_out == 0) {
		return 0;
	}

	xinput_arm_out();

	return consumed;
}

// Control requests reaching the interface (recipient = interface).
// Nothing meaningful to implement; acknowledge so the host never stalls.
static bool xinput_control_xfer_cb(uint8_t rhport,
                                   uint8_t stage,
                                   tusb_control_request_t const *request)
{
	(void) rhport;
	(void) stage;
	(void) request;
	return true;
}

static bool xinput_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	(void) rhport;
	(void) result;

	if (ep_addr == xinput_ep_out) {
		if (xferred_bytes > 0) {
			xinput_handle_out_packet(xinput_out_buf, (uint16_t) xferred_bytes);
		}
		// Re-arm the OUT endpoint so the next LED/rumble packet can arrive.
		xinput_arm_out();
	}

	return true;
}

static const usbd_class_driver_t xinput_driver = {
#if CFG_TUSB_DEBUG >= 2
	.name = "XInput",
#endif
	.init = xinput_init,
	.reset = xinput_reset,
	.open = xinput_open,
	.control_xfer_cb = xinput_control_xfer_cb,
	.xfer_cb = xinput_xfer_cb,
	.sof = NULL,
};

// Called by the device stack to install application-specific drivers.
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = 1;
	return &xinput_driver;
}

//--------------------------------------------------------------------+
// Report sending (device -> host)
//--------------------------------------------------------------------+

bool xinput_send_report(XInputReport const *report)
{
	if (!tud_ready()) {
		return false;
	}
	if (xinput_ep_in == 0) {
		return false; // not enumerated / interface not opened yet
	}
	if (usbd_edpt_busy(0, xinput_ep_in)) {
		return false;
	}
	if (!usbd_edpt_claim(0, xinput_ep_in)) {
		return false;
	}
	// The report is a const struct; usbd_edpt_xfer() takes a non-const
	// pointer but only reads from it for an IN transfer, so the cast is safe.
	if (!usbd_edpt_xfer(0, xinput_ep_in, (uint8_t *) report, XINPUT_REPORT_SIZE)) {
		usbd_edpt_release(0, xinput_ep_in);
		return false;
	}
	return true;
}
