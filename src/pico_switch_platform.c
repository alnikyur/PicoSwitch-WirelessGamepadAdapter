#include <stdio.h>
#include <string.h>
#include <btstack_run_loop.h>
#include <hardware/gpio.h>
#include <pico/time.h>

#include <pico/cyw43_arch.h>
#include <pico/multicore.h>
#include <pico/async_context.h>
#include <uni.h>

#include "sdkconfig.h"
#include "uni_hid_device.h"
#include "uni_log.h"
#include "usb.h"
#include "report.h"
#include "SwitchDescriptors.h"

// Sanity check
#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

#define AXIS_DEADZONE 0xa

// --------------------------------------------------------------------------
// Конфигурация пинов управления ПК
// --------------------------------------------------------------------------
#define POWER_BTN_GPIO 15   // База транзистора C1815 (Active-HIGH, 1 = нажато)
#define PC_SENSE_GPIO  14   // Пин отслеживания PLED+ с колодки JFP1 (1 = ПК включен)

#define POWER_PRESS_MS 300

static btstack_timer_source_t btn_off_timer;
static bool btn_timer_active = false;

// Инициализация пинов для управления ПК
static void pc_power_control_init(void) {
    // 1. Инициализация пина кнопки (Управление транзистором C1815)
    gpio_init(POWER_BTN_GPIO);
    gpio_set_dir(POWER_BTN_GPIO, GPIO_OUT);
    gpio_pull_down(POWER_BTN_GPIO); // Защита от наводок при загрузке
    gpio_put(POWER_BTN_GPIO, 0);     // 0 = транзистор закрыт, кнопка отпущена

    // 2. Инициализация пина-детектора статуса ПК
    gpio_init(PC_SENSE_GPIO);
    gpio_set_dir(PC_SENSE_GPIO, GPIO_IN);
    gpio_pull_down(PC_SENSE_GPIO);   // Если PLED обесточен -> честный 0
}

// Колбэк таймера: отжимаем кнопку питания через 300 мс
static void btn_off_timer_timeout(btstack_timer_source_t *timer) {
    (void)timer;
    gpio_put(POWER_BTN_GPIO, 0);
    btn_timer_active = false;
    logi("my_platform: Power button RELEASED\n");
}

// Функция безопасного импульса нажатия кнопки
static bool press_power_button_safe(void) {
    // 1. Не нажимаем, если импульс уже подается прямо сейчас
    if (btn_timer_active) {
        logi("my_platform: Button press already in progress...\n");
        return false;
    }

    // 2. Проверяем статус PLED (PC_SENSE_GPIO) с фильтрацией шумов
    int sense_1 = gpio_get(PC_SENSE_GPIO);
    sleep_us(100);
    int sense_2 = gpio_get(PC_SENSE_GPIO);

    // Если на PLED+ действительно есть устойчивое напряжение -> ПК включен
    if (sense_1 == 1 && sense_2 == 1) {
        logi("my_platform: PC is already ON (PC_SENSE = 1), skipping press\n");
        return false;
    }

    // 3. ПК выключен -> подаем импульс на базу C1815 через таймер
    logi("my_platform: PC is OFF, PRESSING power button...\n");
    gpio_put(POWER_BTN_GPIO, 1); // Открываем транзистор

    btstack_run_loop_set_timer_handler(&btn_off_timer, btn_off_timer_timeout);
    btstack_run_loop_set_timer(&btn_off_timer, POWER_PRESS_MS);
    btn_timer_active = true;
    btstack_run_loop_add_timer(&btn_off_timer);

    return true;
}

// Declarations
static void trigger_event_on_gamepad(uni_hid_device_t *d);
SwitchOutReport report[CONFIG_BLUEPAD32_MAX_DEVICES];
SwitchIdxOutReport idx_r;
uint8_t connected_controllers;

// Helper functions
static void empty_gamepad_report(SwitchOutReport *gamepad) {
    gamepad->buttons = 0;
    gamepad->hat = SWITCH_HAT_NOTHING;
    gamepad->lx = SWITCH_JOYSTICK_MID;
    gamepad->ly = SWITCH_JOYSTICK_MID;
    gamepad->rx = SWITCH_JOYSTICK_MID;
    gamepad->ry = SWITCH_JOYSTICK_MID;
}

uint8_t convert_to_switch_axis(int32_t bluepadAxis) {
    // bluepad32 reports from -512 to 511 as int32_t
    // switch reports from 0 to 255 as uint8_t

    bluepadAxis += 513;  // now max possible is 1024
    bluepadAxis /= 4;    // now max possible is 255

    if (bluepadAxis < SWITCH_JOYSTICK_MIN)
        bluepadAxis = 0;
    else if ((bluepadAxis > (SWITCH_JOYSTICK_MID - AXIS_DEADZONE)) &&
             (bluepadAxis < (SWITCH_JOYSTICK_MID + AXIS_DEADZONE))) {
        bluepadAxis = SWITCH_JOYSTICK_MID;
    } else if (bluepadAxis > SWITCH_JOYSTICK_MAX)
        bluepadAxis = SWITCH_JOYSTICK_MAX;

    return (uint8_t) bluepadAxis;
}

static void fill_gamepad_report(int idx, uni_gamepad_t *gp) {
    empty_gamepad_report(&report[idx]);

    // face buttons
    if ((gp->buttons & BUTTON_A)) {
        report[idx].buttons |= SWITCH_MASK_A;
    }
    if ((gp->buttons & BUTTON_B)) {
        report[idx].buttons |= SWITCH_MASK_B;
    }
    if ((gp->buttons & BUTTON_X)) {
        report[idx].buttons |= SWITCH_MASK_X;
    }
    if ((gp->buttons & BUTTON_Y)) {
        report[idx].buttons |= SWITCH_MASK_Y;
    }

    // shoulder buttons
    if ((gp->buttons & BUTTON_SHOULDER_L)) {
        report[idx].buttons |= SWITCH_MASK_L;
    }
    if ((gp->buttons & BUTTON_SHOULDER_R)) {
        report[idx].buttons |= SWITCH_MASK_R;
    }

    // dpad
    switch (gp->dpad) {
    case DPAD_UP:
        report[idx].hat = SWITCH_HAT_UP;
        break;
    case DPAD_DOWN:
        report[idx].hat = SWITCH_HAT_DOWN;
        break;
    case DPAD_LEFT:
        report[idx].hat = SWITCH_HAT_LEFT;
        break;
    case DPAD_RIGHT:
        report[idx].hat = SWITCH_HAT_RIGHT;
        break;
    case DPAD_UP | DPAD_RIGHT:
        report[idx].hat = SWITCH_HAT_UPRIGHT;
        break;
    case DPAD_DOWN | DPAD_RIGHT:
        report[idx].hat = SWITCH_HAT_DOWNRIGHT;
        break;
    case DPAD_DOWN | DPAD_LEFT:
        report[idx].hat = SWITCH_HAT_DOWNLEFT;
        break;
    case DPAD_UP | DPAD_LEFT:
        report[idx].hat = SWITCH_HAT_UPLEFT;
        break;
    default:
        report[idx].hat = SWITCH_HAT_NOTHING;
        break;
    }

    // sticks
    report[idx].lx = convert_to_switch_axis(gp->axis_x);
    report[idx].ly = convert_to_switch_axis(gp->axis_y);
    report[idx].rx = convert_to_switch_axis(gp->axis_rx);
    report[idx].ry = convert_to_switch_axis(gp->axis_ry);
    if ((gp->buttons & BUTTON_THUMB_L))
        report[idx].buttons |= SWITCH_MASK_L3;
    if ((gp->buttons & BUTTON_THUMB_R))
        report[idx].buttons |= SWITCH_MASK_R3;

    // triggers
    if (gp->brake)
        report[idx].buttons |= SWITCH_MASK_ZL;
    if (gp->throttle)
        report[idx].buttons |= SWITCH_MASK_ZR;

    // misc buttons
    if (gp->misc_buttons & MISC_BUTTON_SYSTEM)
        report[idx].buttons |= SWITCH_MASK_HOME;
    if (gp->misc_buttons & MISC_BUTTON_CAPTURE)
        report[idx].buttons |= SWITCH_MASK_CAPTURE;
    if (gp->misc_buttons & MISC_BUTTON_BACK)
        report[idx].buttons |= SWITCH_MASK_MINUS;
    if (gp->misc_buttons & MISC_BUTTON_HOME)
        report[idx].buttons |= SWITCH_MASK_PLUS;
}

static void set_led_status(void) {
    if (connected_controllers == 0)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    else
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
}

//
// Platform Overrides
//
static void pico_switch_platform_init(int argc, const char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    logi("my_platform: init()\n");

    // Инициализируем пины кнопки и детектора статуса ПК
    pc_power_control_init();

    btn_timer_active = false;
    connected_controllers = 0;

    uni_gamepad_mappings_t mappings = GAMEPAD_DEFAULT_MAPPINGS;

    // remaps
    mappings.button_b = UNI_GAMEPAD_MAPPINGS_BUTTON_A;
    mappings.button_a = UNI_GAMEPAD_MAPPINGS_BUTTON_B;
    mappings.button_y = UNI_GAMEPAD_MAPPINGS_BUTTON_X;
    mappings.button_x = UNI_GAMEPAD_MAPPINGS_BUTTON_Y;

    uni_gamepad_set_mappings(&mappings);

    idx_r.idx = 0;
    idx_r.report.buttons = 0;
    idx_r.report.hat = SWITCH_HAT_NOTHING;
    idx_r.report.lx = 0;
    idx_r.report.ly = 0;
    idx_r.report.rx = 0;
    idx_r.report.ry = 0;
    set_global_gamepad_report(&idx_r);
}

static void pico_switch_platform_on_init_complete(void) {
    logi("my_platform: on_init_complete()\n");

    // Start scanning
    uni_bt_enable_new_connections_unsafe(true);

    // НЕ удаляем ключи при каждом старте! (ставим 0)
    if (0)
        uni_bt_del_keys_unsafe();
    else
        uni_bt_list_keys_unsafe();

    // Turn off LED once init is done.
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    logi("BLUEPAD: ready to fill reports\n");
    multicore_fifo_push_blocking(0); // signal other core to start reading
}

static void pico_switch_platform_on_device_connected(uni_hid_device_t* d) {
    logi("my_platform: device connected: %p\n", d);
}

static void pico_switch_platform_on_device_disconnected(uni_hid_device_t* d) {
    logi("my_platform: device disconnected: %p\n", d);
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++) {
        empty_gamepad_report(&report[i]);
        idx_r.idx = i;
        idx_r.report = report[i];
        set_global_gamepad_report(&idx_r);
    }
    if (connected_controllers > 0) {
        connected_controllers--;
    }
    set_led_status();
}

static uni_error_t pico_switch_platform_on_device_ready(uni_hid_device_t* d) {
    logi("my_platform: device ready: %p\n", d);

    // Безопасно включаем ПК при готовности геймпада
    press_power_button_safe();

    connected_controllers++;
    set_led_status();
    return UNI_ERROR_SUCCESS;
}

static void pico_switch_platform_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) {
        return;
    }

    uni_gamepad_t *gp;
    uint8_t idx = uni_hid_device_get_idx_for_instance(d);

    gp = &ctl->gamepad;
    fill_gamepad_report(idx, gp);
    idx_r.idx = idx;
    idx_r.report = report[idx];
    set_global_gamepad_report(&idx_r);
}

static const uni_property_t* pico_switch_platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void pico_switch_platform_on_oob_event(uni_platform_oob_event_t event, void* data) {
    ARG_UNUSED(event);
    ARG_UNUSED(data);
    return;
}

//
// Helpers - UNUSED
//
static void trigger_event_on_gamepad(uni_hid_device_t* d) {
    if (d->report_parser.set_player_leds != NULL) {
        static uint8_t led = 0;
        led += 1;
        led &= 0xf;
        d->report_parser.set_player_leds(d, led);
    }

    if (d->report_parser.set_lightbar_color != NULL) {
        static uint8_t red = 0x10;
        static uint8_t green = 0x20;
        static uint8_t blue = 0x40;

        red += 0x10;
        green -= 0x20;
        blue += 0x40;
        d->report_parser.set_lightbar_color(d, red, green, blue);
    }
}

//
// Entry Point
//
struct uni_platform* get_my_platform(void) {
    static struct uni_platform plat = {
        .name = "My Platform",
        .init = pico_switch_platform_init,
        .on_init_complete = pico_switch_platform_on_init_complete,
        .on_device_connected = pico_switch_platform_on_device_connected,
        .on_device_disconnected = pico_switch_platform_on_device_disconnected,
        .on_device_ready = pico_switch_platform_on_device_ready,
        .on_oob_event = pico_switch_platform_on_oob_event,
        .on_controller_data = pico_switch_platform_on_controller_data,
        .get_property = pico_switch_platform_get_property,
    };

    return &plat;
}