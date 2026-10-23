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
#include "xinput_device.h"

// Sanity check
#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

// --------------------------------------------------------------------------
// Конфигурация пинов управления ПК
// --------------------------------------------------------------------------
#define POWER_BTN_GPIO 15   // База транзистора C1815 (Active-HIGH, 1 = нажато)
#define PC_STATE_PIN 14     // Collector of the external PLED-sense NPN transistor
#define PC_STATE_ACTIVE_LEVEL 1
#define PC_STATE_STABLE_MS 100
// Must exceed the longest HIGH phase of the motherboard's sleep LED.
#define PC_STATE_SLEEP_GUARD_MS 3000
#ifndef PC_POWER_DRY_RUN
#define PC_POWER_DRY_RUN 0  // 1: log decisions, never assert the power-button GPIO
#endif

#if PC_STATE_PIN < 0 || PC_STATE_PIN > 28 || PC_STATE_PIN == 23 || \
    PC_STATE_PIN == 24 || PC_STATE_PIN == 25 || PC_STATE_PIN == POWER_BTN_GPIO
#error "PC_STATE_PIN must be an available Pico W GPIO, distinct from POWER_BTN_GPIO"
#endif
#if defined(PC_STATE_DEBUG_UART) && (PC_STATE_PIN == 0 || PC_STATE_PIN == 1)
#error "PC_STATE_PIN conflicts with diagnostic UART0"
#endif
#if PC_STATE_ACTIVE_LEVEL != 0 && PC_STATE_ACTIVE_LEVEL != 1
#error "PC_STATE_ACTIVE_LEVEL must be 0 or 1"
#endif

#define POWER_PRESS_MS 300
#define PC_CHECK_INTERVAL_MS 10

static btstack_timer_source_t btn_off_timer;
static btstack_timer_source_t pc_status_timer;
static btstack_timer_source_t deferred_disconnect_timer;
static bool deferred_disconnect_timer_active = false;
static bool btn_timer_active = false;
static bool last_pc_state = false; // Храним последнее известное состояние ПК
static bool pc_state_initialized = false; // false, пока не сделано первое измерение
static bool pc_candidate_on;
static uint64_t pc_candidate_since_us;
static bool power_request_pending;
static uint64_t pc_last_health_log_us;

// UNKNOWN means no stable decision yet; never treat it as OFF.
typedef enum { PC_STATE_UNKNOWN, PC_STATE_OFF, PC_STATE_ON } pc_state_t;

static pc_state_t read_pc_state(void) {
    bool on = gpio_get(PC_STATE_PIN) == PC_STATE_ACTIVE_LEVEL;
    uint64_t now = time_us_64();
    if (on != pc_candidate_on) {
        pc_candidate_on = on;
        pc_candidate_since_us = now;
    }
    uint32_t required_ms = PC_STATE_STABLE_MS;
    if (on && required_ms < PC_STATE_SLEEP_GUARD_MS)
        required_ms = PC_STATE_SLEEP_GUARD_MS;
    if (now - pc_candidate_since_us < (uint64_t)required_ms * 1000)
        return PC_STATE_UNKNOWN;
    return on ? PC_STATE_ON : PC_STATE_OFF;
}

static void log_pc_state(const char* event) {
    pc_state_t state = read_pc_state();
    logi("my_platform: %s PC_STATE_PIN=GP%d raw=%d state=%s dry_run=%d\n",
         event, PC_STATE_PIN, gpio_get(PC_STATE_PIN),
         state == PC_STATE_ON ? "ON" : state == PC_STATE_OFF ? "OFF" : "SETTLING",
         PC_POWER_DRY_RUN);
}

// Инициализация пинов для управления ПК
static void pc_power_control_init(void) {
    gpio_init(POWER_BTN_GPIO);
    gpio_set_dir(POWER_BTN_GPIO, GPIO_OUT);
    gpio_pull_down(POWER_BTN_GPIO);
    gpio_put(POWER_BTN_GPIO, 0);

    gpio_init(PC_STATE_PIN);
    gpio_set_dir(PC_STATE_PIN, GPIO_IN);
    gpio_pull_up(PC_STATE_PIN);
    pc_candidate_on = gpio_get(PC_STATE_PIN) == PC_STATE_ACTIVE_LEVEL;
    pc_candidate_since_us = time_us_64();
    pc_state_initialized = false;
    power_request_pending = false;
    log_pc_state("startup");
}

// Колбэк таймера: отжимаем кнопку питания через 300 мс
static void btn_off_timer_timeout(btstack_timer_source_t *timer) {
    (void)timer;
    gpio_put(POWER_BTN_GPIO, 0);
    btn_timer_active = false;
    logi("my_platform: Power button RELEASED\n");
}

// Automatic presses require a freshly confirmed OFF input.
static bool press_power_button_safe(bool force) {
    if (btn_timer_active) {
        logi("my_platform: Button press already in progress...\n");
        return false;
    }

    if (!force) {
        if (read_pc_state() != PC_STATE_OFF) {
            logi("my_platform: PC is ON or SETTLING, skipping press\n");
            return false;
        }
    }

    if (PC_POWER_DRY_RUN) {
        logi("my_platform: DRY RUN: would press power button (force=%d)\n", force);
        return true;
    }
    logi("my_platform: PRESSING power button (force=%d)\n", force);
    gpio_put(POWER_BTN_GPIO, 1);

    btstack_run_loop_set_timer_handler(&btn_off_timer, btn_off_timer_timeout);
    btstack_run_loop_set_timer(&btn_off_timer, POWER_PRESS_MS);
    btn_timer_active = true;
    btstack_run_loop_add_timer(&btn_off_timer);

    return true;
}

// Функция отключения всех подключенных геймпадов.
//
// Вызывается из btstack-таймера; отчёты защищены отдельной короткой блокировкой.
static void disconnect_all_controllers(void) {
    logi("my_platform: PC turned OFF/SLEEP! Disconnecting gamepad(s)...\n");
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++) {
        uni_hid_device_t* d = uni_hid_device_get_instance_for_idx(i);
        if (d && uni_hid_device_is_gamepad(d)) {
            uni_hid_device_disconnect(d);
        }
    }
}

// Отложенный вызов disconnect_all_controllers().
// Выполняется отдельным таймером ПОСЛЕ возврата из pc_status_timer_timeout(),
// чтобы не разбирать список устройств прямо во время обработки другого таймера.
static void deferred_disconnect_timer_timeout(btstack_timer_source_t *timer) {
    (void)timer;
    deferred_disconnect_timer_active = false;
    // A wake event can supersede a queued shutdown callback.
    if (pc_state_initialized && last_pc_state)
        return;

    // Only disconnect Bluetooth; the neutral report keeps the USB pad present.
    disconnect_all_controllers();
}

static void schedule_disconnect_all_controllers(void) {
    if (deferred_disconnect_timer_active) {
        // Уже запланировано — повторная регистрация того же timer_source
        // приводит к btstack_assert(false) и зависанию прошивки.
        return;
    }

    deferred_disconnect_timer_active = true;
    btstack_run_loop_set_timer_handler(&deferred_disconnect_timer,
                                       deferred_disconnect_timer_timeout);
    btstack_run_loop_set_timer(&deferred_disconnect_timer, 0);
    btstack_run_loop_add_timer(&deferred_disconnect_timer);
}

// Периодический мониторинг состояния ПК
static void pc_status_timer_timeout(btstack_timer_source_t *timer) {
    pc_state_t state = read_pc_state();
    if (state == PC_STATE_UNKNOWN)
        goto rearm;
    bool current_pc_state = state == PC_STATE_ON;
    if (!pc_state_initialized || current_pc_state != last_pc_state)
        log_pc_state("confirmed");

    if (power_request_pending) {
        power_request_pending = false;
        if (state == PC_STATE_OFF)
            press_power_button_safe(false);
        else {
            logi("my_platform: PC is ON, skipping automatic power press\n");
        }
    }

    // Детектируем момент ВЫКЛЮЧЕНИЯ ПК (был 1, стал 0).
    // Только при первом измерении НЕ сравниваем: иначе выключенный на момент
    // старта ПК сразу даст ложное событие.
    if (pc_state_initialized && last_pc_state && !current_pc_state) {
        logi("my_platform: Detected PC Shutdown/Sleep event!\n");
        schedule_disconnect_all_controllers();
    }

    // Cancel stale Bluetooth shutdown work without touching USB enumeration.
    if (current_pc_state) {
        btstack_run_loop_remove_timer(&deferred_disconnect_timer);
        deferred_disconnect_timer_active = false;
    }

    last_pc_state = current_pc_state;
    pc_state_initialized = true;

    // Перезапускаем таймер проверки.
    // remove+add: таймер снимается process_timers() перед вызовом колбэка,
    // но remove() защищает от случайной повторной регистрации.
rearm:
    if (time_us_64() - pc_last_health_log_us >= 60000000) {
        pc_last_health_log_us = time_us_64();
        log_pc_state("power monitor alive");
    }
    btstack_run_loop_remove_timer(timer);
    btstack_run_loop_set_timer(timer, PC_CHECK_INTERVAL_MS);
    btstack_run_loop_add_timer(timer);
}

// Declarations
static void trigger_event_on_gamepad(uni_hid_device_t *d);
XInputReport report[CONFIG_BLUEPAD32_MAX_DEVICES];
XInputIdxReport idx_r;
uint8_t connected_controllers;

// --------------------------------------------------------------------------
// Маппинг Bluepad32 -> XInput
// --------------------------------------------------------------------------
//
// Соответствие кнопок (для проверки):
//
//  Bluepad32                      XInput byte2/byte3
//  -----------------------------  -------------------------------------
//  DPAD_UP                        byte2 0x01  (D-pad Up)
//  DPAD_DOWN                      byte2 0x02  (D-pad Down)
//  DPAD_LEFT                      byte2 0x04  (D-pad Left)
//  DPAD_RIGHT                     byte2 0x08  (D-pad Right)
//  MISC_BUTTON_START  (+)         byte2 0x10  (Start / Menu)
//  MISC_BUTTON_SELECT (-)         byte2 0x20  (Back / View)
//  BUTTON_THUMB_L                 byte2 0x40  (L3)
//  BUTTON_THUMB_R                 byte2 0x80  (R3)
//  BUTTON_SHOULDER_L              byte3 0x01  (LB)
//  BUTTON_SHOULDER_R              byte3 0x02  (RB)
//  MISC_BUTTON_SYSTEM (Home/PS)   byte3 0x04  (Guide / Xbox)
//  BUTTON_A                       byte3 0x10  (A)
//  BUTTON_B                       byte3 0x20  (B)
//  BUTTON_X                       byte3 0x40  (X)
//  BUTTON_Y                       byte3 0x80  (Y)
//
//  brake    (0..1023)             byte4     (left trigger, 0..255)
//  throttle (0..1023)             byte5     (right trigger, 0..255)
//  axis_x   (-512..511)           bytes 6-7 (LX, int16 LE)
//  axis_y   (-512..511)           bytes 8-9 (LY, int16 LE, inverted)
//  axis_rx  (-512..511)           bytes 10-11 (RX, int16 LE)
//  axis_ry  (-512..511)           bytes 12-13 (RY, int16 LE, inverted)
//
// Примечания:
//  * BUTTON_TRIGGER_L / BUTTON_TRIGGER_R (цифровые "нажатия" триггеров)
//    отдельно не обрабатываются: триггеры передаются как аналоговые
//    значения из brake/throttle. Если у геймпада нет аналоговых триггеров,
//    стоить добавить их как LB/RB.
//  * MISC_BUTTON_CAPTURE не имеет прямого аналога на Xbox 360, поэтому
//    игнорируется.
//  * bytes 14-19 остаются нулями (зарезервированы).

#define XINPUT_TRIGGER_MAX 1023

// 0..1023 -> 0..255
static uint8_t trigger_to_xinput(int32_t value) {
    if (value <= 0)
        return 0;
    if (value > XINPUT_TRIGGER_MAX)
        value = XINPUT_TRIGGER_MAX;
    return (uint8_t) (value * 255 / XINPUT_TRIGGER_MAX);
}

// -512..511 -> -32768..32767, с инверсией (для Y-осей) и ограничением диапазона.
//
// ВАЖНО: все вычисления и инверсия делаются в int32, а в int16 значение
// приводится только после ограничения. Раньше значение 512 * 64 = 32768 и
// инверсия -(-32768) = 32768 переполняли int16 и меняли знак на краю хода
// стика (стик "перескакивал" на противоположную сторону).
static int16_t axis_to_xinput(int32_t value, bool invert) {
    if (value > 511)
        value = 511;
    if (value < -512)
        value = -512;

    // -512 -> -32768, 0 -> 0, 511 -> 32767
    int32_t out = (value >= 0) ? (value * 32767) / 511 : value * 64;

    if (invert)
        out = -out;

    if (out > 32767)
        out = 32767;
    if (out < -32768)
        out = -32768;

    return (int16_t) out;
}

static void empty_gamepad_report(XInputReport *gamepad) {
    memset(gamepad, 0, sizeof(*gamepad));
    gamepad->header0 = XINPUT_REPORT_HEADER_0;
    gamepad->header1 = XINPUT_REPORT_HEADER_1;
}

static void fill_gamepad_report(int idx, uni_gamepad_t *gp) {
    XInputReport *r = &report[idx];

    empty_gamepad_report(r);

    // D-pad
    if (gp->dpad & DPAD_UP)
        r->buttons2 |= XINPUT_DPAD_UP;
    if (gp->dpad & DPAD_DOWN)
        r->buttons2 |= XINPUT_DPAD_DOWN;
    if (gp->dpad & DPAD_LEFT)
        r->buttons2 |= XINPUT_DPAD_LEFT;
    if (gp->dpad & DPAD_RIGHT)
        r->buttons2 |= XINPUT_DPAD_RIGHT;

    // Start / Back / thumb clicks
    if (gp->misc_buttons & MISC_BUTTON_START)
        r->buttons2 |= XINPUT_BUTTON_START;
    if (gp->misc_buttons & MISC_BUTTON_SELECT)
        r->buttons2 |= XINPUT_BUTTON_BACK;
    if (gp->buttons & BUTTON_THUMB_L)
        r->buttons2 |= XINPUT_BUTTON_L3;
    if (gp->buttons & BUTTON_THUMB_R)
        r->buttons2 |= XINPUT_BUTTON_R3;

    // Shoulders / guide / face buttons
    if (gp->buttons & BUTTON_SHOULDER_L)
        r->buttons3 |= XINPUT_BUTTON_LB;
    if (gp->buttons & BUTTON_SHOULDER_R)
        r->buttons3 |= XINPUT_BUTTON_RB;
    if (gp->misc_buttons & MISC_BUTTON_SYSTEM)
        r->buttons3 |= XINPUT_BUTTON_GUIDE;
    if (gp->buttons & BUTTON_A)
        r->buttons3 |= XINPUT_BUTTON_A;
    if (gp->buttons & BUTTON_B)
        r->buttons3 |= XINPUT_BUTTON_B;
    if (gp->buttons & BUTTON_X)
        r->buttons3 |= XINPUT_BUTTON_X;
    if (gp->buttons & BUTTON_Y)
        r->buttons3 |= XINPUT_BUTTON_Y;

    // Analog triggers
    r->trigger_l = trigger_to_xinput(gp->brake);
    r->trigger_r = trigger_to_xinput(gp->throttle);

    // Analog sticks. Y axes are inverted: Bluepad32 reports "up" as negative,
    // XInput reports "up" as positive.
    r->axis_lx = axis_to_xinput(gp->axis_x, false);
    r->axis_ly = axis_to_xinput(gp->axis_y, true);
    r->axis_rx = axis_to_xinput(gp->axis_rx, false);
    r->axis_ry = axis_to_xinput(gp->axis_ry, true);
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

    pc_power_control_init();

    btn_timer_active = false;
    connected_controllers = 0;
    pc_state_initialized = false;
    deferred_disconnect_timer_active = false;

    uni_gamepad_mappings_t mappings = GAMEPAD_DEFAULT_MAPPINGS;

    mappings.button_a = UNI_GAMEPAD_MAPPINGS_BUTTON_A;
    mappings.button_b = UNI_GAMEPAD_MAPPINGS_BUTTON_B;
    mappings.button_y = UNI_GAMEPAD_MAPPINGS_BUTTON_Y;
    mappings.button_x = UNI_GAMEPAD_MAPPINGS_BUTTON_X;

    uni_gamepad_set_mappings(&mappings);

    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++) {
        empty_gamepad_report(&report[i]);
    }

    idx_r.idx = 0;
    empty_xinput_report(&idx_r);
    set_global_gamepad_report(&idx_r);
}

static void pico_switch_platform_on_init_complete(void) {
    logi("my_platform: on_init_complete()\n");

    uni_bt_enable_new_connections_unsafe(true);

    if (0)
        uni_bt_del_keys_unsafe();
    else
        uni_bt_list_keys_unsafe();

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    // Неблокирующий опрос состояния ПК каждые PC_CHECK_INTERVAL_MS.
    btstack_run_loop_set_timer_handler(&pc_status_timer, pc_status_timer_timeout);
    btstack_run_loop_set_timer(&pc_status_timer, PC_CHECK_INTERVAL_MS);
    btstack_run_loop_add_timer(&pc_status_timer);

    logi("BLUEPAD: ready to fill reports\n");
}

static void pico_switch_platform_on_device_connected(uni_hid_device_t* d) {
    logi("my_platform: device connected: %p\n", d);
    log_pc_state("gamepad connected");
}

static void pico_switch_platform_on_device_disconnected(uni_hid_device_t* d) {
    logi("my_platform: device disconnected: %p\n", d);
    
    power_request_pending = false;
    btstack_run_loop_remove_timer(&btn_off_timer);
    btn_timer_active = false;
    gpio_put(POWER_BTN_GPIO, 0); // Безопасное отключение транзистора

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

    // Безопасно включаем/разбуживаем ПК при готовности геймпада (false = не принудительно)
    log_pc_state("gamepad ready");
    // Wait for stable LOW or HIGH in the polling timer without blocking BTstack.
    power_request_pending = true;

    connected_controllers++;
    set_led_status();
    return UNI_ERROR_SUCCESS;
}

static void pico_switch_platform_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) {
        return;
    }

    uni_gamepad_t *gp = &ctl->gamepad;

    // Ручная комбинация принудительного старта: L1 + R1 + Home (System)
    if ((gp->buttons & BUTTON_SHOULDER_L) && 
        (gp->buttons & BUTTON_SHOULDER_R) && 
        (gp->misc_buttons & MISC_BUTTON_SYSTEM)) {
        logi("my_platform: Force power press combo activated!\n");
        press_power_button_safe(true); // true = принудительно, независимо от PLED
    }

    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
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
