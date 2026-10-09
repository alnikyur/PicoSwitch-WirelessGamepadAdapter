# WakePad360

Use any Bluetooth gamepad as a **wired Xbox 360 controller** on your PC, with a Raspberry Pi Pico W.

A gamepad pairs over Bluetooth and its input is forwarded to the host as a standard XInput
device, so no driver installation is needed. While it is paired, the gamepad can also power
the PC on and off.

[List of supported controllers](https://bluepad32.readthedocs.io/en/latest/supported_gamepads/)

This project is possible thanks to [Bluepad32](https://github.com/ricardoquesada/bluepad32) and [TinyUSB](https://github.com/hathach/tinyusb).

## Features

- Bluetooth gamepad to Xbox 360 / XInput over USB.
- Full button, D-pad, analog stick and analog trigger mapping.
- On-board LED lights up while at least one gamepad is connected.
- The gamepad can start or wake the PC, and the emulated controller disappears from the
  host when the PC shuts down or goes to sleep.
- Manual force-power combo: `L1 + R1 + Home`.

## How it works

Both RP2040 cores have one job each:

| Core | Task | Responsibility |
| ---- | ---- | -------------- |
| core1 | `bluepad_core_task()` | cyw43/Bluetooth, Bluepad32 (`uni_*`) and the BTstack run loop. All gamepad callbacks and timers run here. |
| core0 | `usb_core_task()` | TinyUSB device stack. Runs `tud_task()` and sends one XInput report every 4 ms. |

Gamepad samples travel from core1 to core0 through `shared_report` in `src/report.c`: core1
takes the cyw43 async context lock, copies the report and pushes the multicore FIFO; core0
pops the FIFO and reads the report under the same lock. Attach/detach requests use a
separate flag (`usb_request_attach()`), because TinyUSB may only be driven from the core
that runs `tud_task()`.

The Xbox 360 emulation is a custom TinyUSB class driver (`src/xinput_device.c`), not HID:
VID/PID `0x045E:0x028E`, vendor interface `0xFF/0x5D/0x01`, interrupt IN `0x81` and OUT
`0x01`, and a 20-byte report. No built-in TinyUSB class is enabled, so the device exposes
a single interface.

### Host compatibility

| Host | Status |
| ---- | ------ |
| Linux (`xpad`, Steam Input) | Works. |
| Windows (Xbox 360 driver) | Not supported: Windows requires the `0x40`/`0x41` authentication handshake, which is not implemented, so `xusb22.sys` does not enumerate the device. |
| Real Xbox 360 console | Not supported, for the same reason. |

## Hardware

Only two GPIOs are used beyond the USB connection:

| Pico W pin | Direction | Purpose |
| ---------- | --------- | ------- |
| `GPIO 15` | Output | Drives the base of an NPN transistor (C1815) wired across the PC's power-button pins. HIGH = button pressed. |
| `GPIO 14` | Input (pull-down) | Reads the PC power LED (`PLED+` from the front-panel header). `1` = PC on. |

`GPIO 15` is driven LOW and has a pull-down at boot, so the transistor stays off while the
Pico resets.

## Power control

The firmware polls the power-sense pin once per second:

- **PC powered on** (`0 -> 1`): the emulated controller is re-attached to the USB bus, so
  the host sees the gamepad again.
- **PC shut down or went to sleep** (`1 -> 0`): the emulated controller is detached from
  the host and every connected gamepad is disconnected.
- **A gamepad becomes ready** and the PC is not fully on: a 300 ms power-button press is
  issued to start or wake the PC. `is_pc_fully_on()` takes eight samples 50 us apart, so
  the blinking power LED of a sleeping PC is not mistaken for "on".
- **`L1 + R1 + Home`** forces a 300 ms press even when the firmware believes the PC is
  already on.

## Button mapping

| Bluepad32 | XInput | Notes |
| --------- | ------ | ----- |
| `DPAD_UP` / `DOWN` / `LEFT` / `RIGHT` | `buttons2` `0x01` / `0x02` / `0x04` / `0x08` | D-pad |
| `MISC_BUTTON_START` (`+`) | `buttons2` `0x10` | Menu |
| `MISC_BUTTON_SELECT` (`-`) | `buttons2` `0x20` | View |
| `BUTTON_THUMB_L` / `BUTTON_THUMB_R` | `buttons2` `0x40` / `0x80` | L3 / R3 |
| `BUTTON_SHOULDER_L` / `BUTTON_SHOULDER_R` | `buttons3` `0x01` / `0x02` | LB / RB |
| `MISC_BUTTON_SYSTEM` | `buttons3` `0x04` | Guide / Home |
| `BUTTON_A` / `BUTTON_B` / `BUTTON_X` / `BUTTON_Y` | `buttons3` `0x10` / `0x20` / `0x40` / `0x80` | |
| `brake` / `throttle` | `trigger_l` / `trigger_r` | `0..1023` scaled to `0..255` |
| `axis_x`, `axis_rx` | `axis_lx`, `axis_rx` | left/right stick X |
| `axis_y`, `axis_ry` | `axis_ly`, `axis_ry` | inverted: Bluepad32 reports up as negative, XInput as positive |

A/B/X/Y are normalized through `uni_gamepad_set_mappings()` so the physical layout matches
the Xbox labelling. Digital trigger buttons (`BUTTON_TRIGGER_L` / `BUTTON_TRIGGER_R`) are
not mapped separately, because the triggers are sent as analog values. `MISC_BUTTON_CAPTURE`
has no Xbox 360 equivalent and is ignored.

## Repository layout

| Path | Contents |
| ---- | -------- |
| `src/main.c` | Entry point: starts core1, then runs the USB task on core0. |
| `src/pico_switch_platform.c` | Bluepad32 custom platform: device callbacks, Bluepad32 to XInput mapping, PC power control. |
| `src/report.c` | Cross-core report hand-off (async context lock + multicore FIFO). |
| `src/usb.c` | core0 report pump and thread-safe attach/detach requests. |
| `src/xinput_device.c` | Custom TinyUSB class driver for the Xbox 360 interface. |
| `src/usb_descriptors.c` | USB device, configuration and string descriptors. |
| `src/sdkconfig.h` | Bluepad32 configuration (4 devices max, custom platform, log level). |
| `src/btstack_config.h` | BTstack configuration. |
| `include/XInputDescriptors.h` | Report layout, button masks, VID/PID and USB descriptors. |
| `include/report.h`, `include/usb.h`, `include/xinput_device.h` | Interfaces for the modules above. |
| `include/tusb_config.h` | TinyUSB configuration (no built-in class enabled). |

## Installing

1. Download the latest `.uf2` file from [releases](https://github.com/alnikyur/PicoSwitch-WirelessGamepadAdapter/releases).
2. Hold the BOOTSEL button while plugging the Pico W into your PC.
3. A mass storage device named `RPI-RP2` appears. Drag and drop the `.uf2` file onto it.

The Pico W reboots by itself. Pair the gamepad as usual; the firmware always accepts new
Bluetooth connections.

## Building

1. Install Make, CMake (at least version 3.13) and the GCC cross compiler:

```bash
sudo apt-get install make cmake gcc-arm-none-eabi build-essential
```

2. (Optional) Install the [Pico SDK](https://github.com/raspberrypi/pico-sdk) and set the
   `PICO_SDK_PATH` environment variable to the SDK path. Without it, the SDK is downloaded
   automatically for each build.
3. Update the submodules:

```bash
make update
```

4. Build:

```bash
make build
```

`PICO_BOARD=pico_w` is required; the build fails without it, because this firmware is
specific to the Pico W.

5. Flash by copying `build/PicoSwitchWGA.uf2` onto the `RPI-RP2` mass storage device, or on
   Linux:

```bash
make flash
```

Without Make, the equivalent CMake invocation is:

```bash
cmake -S . -B build -DPICO_BOARD=pico_w
cmake --build build -j
```

#### Other `make` commands

- `clean` - Remove the build directory.
- `flash_nuke` - Flash `flash_nuke.uf2` to erase the Pico's flash memory. Useful when the
  Pico is stuck in a boot loop.
- `all` - `build` followed by `flash`.
- `format` - Format the code with `clang-format`. Requires `clang-format`.
- `update` - Initialize and update the `bluepad32` submodule and the SDK's submodules.
- `debug` - Open `minicom` on `/dev/ttyACM0`. Requires `minicom` and UART stdio, which is
  disabled by default in `CMakeLists.txt`.

### Notes

- USB stdio must stay disabled. Enabling it would pull in a CDC class driver next to the
  Xbox 360 driver and break the single-interface device. UART stdio is disabled as well.
- For the first 5 seconds after boot, neutral reports are sent, so the host always sees a
  live device even before a gamepad connects.
- Reports are sent every 4 ms, matching `bInterval` in the IN endpoint descriptor.
- Diagnostic build: compile with `-DUSB_SELFTEST=1` to bypass Bluepad32 and toggle button A
  once per second. If A blinks on the host, the USB path works and the problem is on the
  Bluetooth side.
- Up to 4 gamepads can pair (`CONFIG_BLUEPAD32_MAX_DEVICES`), but they feed a single
  emulated controller: the most recent sample wins.
- LED and rumble commands sent by the host are parsed and stored, but not yet forwarded to
  the gamepad.

## Acknowledgements

- [ricardoquesada](https://github.com/ricardoquesada) - maker of [Bluepad32](https://github.com/ricardoquesada/bluepad32)
- [hathach](https://github.com/hathach) - creator of [TinyUSB](https://github.com/hathach/tinyusb)
- [splork](https://github.com/aveao/splork) and [retro-pico-switch](https://github.com/DavidPagels/retro-pico-switch) - for the HID descriptors and TinyUSB usage examples
- [juan518munoz](https://github.com/juan518munoz) - original author of this project, which emulated a Nintendo Switch Pro Controller

## License

Licensed under the [Apache License 2.0](./LICENSE).
