/*
 * Minimal Xbox 360 (XInput) wired controller emulation.
 *
 * This replaces the old Nintendo Switch Pro Controller HID descriptors.
 * It is intentionally NOT a real Xbox 360 controller: the 0x40/0x41
 * authentication used by the real Xbox 360 console is not implemented.
 * That is fine for Linux (xpad driver), Steam Input and PC use in general.
 * A real Xbox 360 console will reject this device.
 */

#pragma once

#include <stdint.h>

//--------------------------------------------------------------------+
// XInput report
//--------------------------------------------------------------------+

// Wired Xbox 360 report is 20 bytes.
#define XINPUT_REPORT_SIZE 20
#define XINPUT_ENDPOINT_SIZE 32

// Report header bytes (fixed for the "wired" XInput packet layout).
#define XINPUT_REPORT_HEADER_0 0x00
#define XINPUT_REPORT_HEADER_1 0x14

// byte2: digital buttons / dpad
#define XINPUT_DPAD_UP 0x01
#define XINPUT_DPAD_DOWN 0x02
#define XINPUT_DPAD_LEFT 0x04
#define XINPUT_DPAD_RIGHT 0x08
#define XINPUT_BUTTON_START 0x10  // "Start" / Menu
#define XINPUT_BUTTON_BACK 0x20   // "Back" / View
#define XINPUT_BUTTON_L3 0x40
#define XINPUT_BUTTON_R3 0x80

// byte3: buttons
#define XINPUT_BUTTON_LB 0x01
#define XINPUT_BUTTON_RB 0x02
#define XINPUT_BUTTON_GUIDE 0x04  // Xbox / Guide
#define XINPUT_BUTTON_A 0x10
#define XINPUT_BUTTON_B 0x20
#define XINPUT_BUTTON_X 0x40
#define XINPUT_BUTTON_Y 0x80

typedef struct __attribute__((packed, aligned(1))) {
	uint8_t header0;   // 0x00
	uint8_t header1;   // 0x14
	uint8_t buttons2;  // dpad + start/back + L3/R3
	uint8_t buttons3;  // LB/RB/guide + face buttons
	uint8_t trigger_l; // 0..255
	uint8_t trigger_r; // 0..255
	int16_t axis_lx;   // little endian
	int16_t axis_ly;   // little endian
	int16_t axis_rx;   // little endian
	int16_t axis_ry;   // little endian
	uint8_t reserved[6];
} XInputReport;

_Static_assert(sizeof(XInputReport) == XINPUT_REPORT_SIZE,
               "XInputReport must be exactly 20 bytes");

//--------------------------------------------------------------------+
// USB descriptors
//--------------------------------------------------------------------+

#define XINPUT_EP_IN 0x81
#define XINPUT_EP_OUT 0x01

// Device descriptor: class/subclass/protocol 0xFF (vendor specific).
// VID/PID match the real wired Xbox 360 controller so that Linux's xpad
// driver (and Windows' built-in XInput driver) bind to it.
static const uint8_t xinput_device_descriptor[] = {
	0x12,       // bLength
	0x01,       // bDescriptorType (Device)
	0x00, 0x02, // bcdUSB 2.00
	0xFF,       // bDeviceClass (vendor specific)
	0xFF,       // bDeviceSubClass
	0xFF,       // bDeviceProtocol
	0x40,       // bMaxPacketSize0 64 (must match CFG_TUD_ENDPOINT0_SIZE)
	0x5E, 0x04, // idVendor 0x045E (Microsoft)
	0x8E, 0x02, // idProduct 0x028E (Xbox 360 Controller)
	0x14, 0x01, // bcdDevice 0x0114
	0x01,       // iManufacturer (String Index)
	0x02,       // iProduct (String Index)
	0x00,       // iSerialNumber (none)
	0x01,       // bNumConfigurations 1
};

_Static_assert(sizeof(xinput_device_descriptor) == 18,
               "device descriptor must be 18 bytes");

// Interface-specific vendor descriptor (bDescriptorType 0x21), exactly 17
// bytes. Standard dump from a wired Xbox 360 controller, interface 0.
// xpad does not parse it, but it is kept for compatibility.
#define XINPUT_VENDOR_DESC_LEN 17

#define XINPUT_VENDOR_DESC_BYTES                                              \
	0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, \
	    0x13, 0x01, 0x08, 0x00, 0x00

// Total configuration descriptor length:
//   config 9 + interface 9 + vendor 17 + IN ep 7 + OUT ep 7 = 49
#define XINPUT_CONFIG_TOTAL_LEN 49

static const uint8_t xinput_configuration_descriptor[] = {
	// Configuration descriptor
	0x09,                          // bLength
	0x02,                          // bDescriptorType (Configuration)
	XINPUT_CONFIG_TOTAL_LEN, 0x00, // wTotalLength 49
	0x01,                          // bNumInterfaces 1
	0x01,                          // bConfigurationValue
	0x00,                          // iConfiguration
	0xA0,                          // bmAttributes (bus powered, remote wakeup)
	0xFA,                          // bMaxPower 500mA

	// Interface descriptor
	0x09, // bLength
	0x04, // bDescriptorType (Interface)
	0x00, // bInterfaceNumber 0
	0x00, // bAlternateSetting
	0x02, // bNumEndpoints 2
	0xFF, // bInterfaceClass (vendor specific)
	0x5D, // bInterfaceSubClass (Xbox 360)
	0x01, // bInterfaceProtocol (Xbox 360)
	0x00, // iInterface

	// Interface-specific vendor descriptor (17 bytes)
	XINPUT_VENDOR_DESC_BYTES,

	// Endpoint IN (device -> host: XInput reports)
	0x07,           // bLength
	0x05,           // bDescriptorType (Endpoint)
	XINPUT_EP_IN,   // bEndpointAddress (IN, ep 1)
	0x03,           // bmAttributes (Interrupt)
	0x20, 0x00,     // wMaxPacketSize 32
	0x04,           // bInterval 4 ms

	// Endpoint OUT (host -> device: LED / rumble)
	0x07,           // bLength
	0x05,           // bDescriptorType (Endpoint)
	XINPUT_EP_OUT,  // bEndpointAddress (OUT, ep 1)
	0x03,           // bmAttributes (Interrupt)
	0x20, 0x00,     // wMaxPacketSize 32
	0x08,           // bInterval 8 ms
};

// Catches any length mismatch at build time instead of on the hardware.
_Static_assert(sizeof(xinput_configuration_descriptor) == XINPUT_CONFIG_TOTAL_LEN,
               "configuration descriptor length does not match wTotalLength");

//--------------------------------------------------------------------+
// String descriptors
//--------------------------------------------------------------------+

static const uint8_t xinput_string_language[] = {0x09, 0x04};

// String index N maps to xinput_string_descriptors[N - 1], matching the
// indices used in the device descriptor (iManufacturer = 1, iProduct = 2).
static const char *const xinput_string_descriptors[] = {
	"Microsoft",  // index 1 -> manufacturer
	"Controller", // index 2 -> product
	"1.0",        // index 3 -> version (unused)
};