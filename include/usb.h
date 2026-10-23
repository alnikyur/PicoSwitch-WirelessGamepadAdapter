#ifndef _USB_H_
#define _USB_H_

// USB remains attached; PC/BT state never requests software unplug/replug.
void usb_core_task(void);

#endif
