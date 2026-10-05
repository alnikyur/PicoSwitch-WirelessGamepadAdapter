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
#define PC_SENSE_GPIO  14   // Пин отслеживания PLED+ с колодки JFP1 (1 = ПК включен)

#define POWER_PRESS_MS 300
#define PC_CHECK_INTERVAL_MS 1000 // Проверка статуса ПК каждую секунду

static btstack_timer_source_t btn_off_timer;
static btstack_timer_source_t pc_status_timer;
static btstack_timer_source_t deferred_disconnect_timer;
static bool deferred_disconnect_timer_active = false;
static bool btn_timer_active = false;
static bool last_pc_state = false; // Храним последнее известное состояние ПК
static bool pc_state_initialized = false; // false, пока не сделано первое измерение

// true, когда ПК выключен/спит и геймпады отключены.
// Пока этот флаг установлен, USB-устройство отсоединено от хоста,
// поэтому ПК реально «теряет» геймпад, а не продолжает слать репорты.
static bool pc_off = false;

// Инициализация пинов для управления ПК
static void pc_power_control_init(void) {
    gpio_init(POWER_BTN_GPIO);
    gpio_set_dir(POWER_BTN_GPIO, GPIO_OUT);
    gpio_pull_down(POWER_BTN_GPIO);
    gpio_put(POWER_BTN_GPIO, 0);

    gpio_init(PC_SENSE_GPIO);
    gpio_set_dir(PC_SENSE_GPIO, GPIO_IN);
    gpio_pull_down(PC_SENSE_GPIO);

    // Начальное считывание состояния ПК
    last_pc_state = (gpio_get(PC_SENSE_GPIO) == 1);
    pc_state_initialized = true;
}

// Колбэк таймера: отжимаем кнопку питания через 300 мс
static void btn_off_timer_timeout(btstack_timer_source_t *timer) {
    (void)timer;
    gpio_put(POWER_BTN_GPIO, 0);
    btn_timer_active = false;
    logi("my_platform: Power button RELEASED\n");
}

// Неблокирующая проверка «ПК действительно включён».
//
// Раньше здесь было 10 замеров с sleep_ms(50) прямо на потоке run loop
// (500 мс). Это блокировало обработку BTstack/cyw43, из-за чего отложенный
// таймер отключения успевал зарегистрироваться повторно (btstack_assert),
// и прошивка переставала реагировать на выключение ПК.
//
// Теперь мы просто снимаем несколько быстрых отсчётов подряд (микросекунды),
// а длинное усреднение делает периодический таймер pc_status_timer.
static bool is_pc_fully_on(void) {
    // 8 быстрых отсчётов подряд: у включённого ПК PLED горит непрерывно,
    // у спящего — мигает, поэтому любой ноль означает «не включён».
    for (int i = 0; i < 8; i++) {
        if (gpio_get(PC_SENSE_GPIO) != 1) {
            return false;
        }
        sleep_us(50);
    }
    return true;
}

// Функция безопасного импульса нажатия кнопки
static bool press_power_button_safe(bool force) {
    if (btn_timer_active && !force) {
        logi("my_platform: Button press already in progress...\n");
        return false;
    }

    // Если не просили принудительно — проверяем, не включен ли ПК.
    // Если ПК уже помечен как выключенный (pc_off), не трогаем кнопку:
    // включение выполняется только по явному событию готовности геймпада.
    if (!force) {
        if (!pc_off && is_pc_fully_on()) {
            logi("my_platform: PC is FULLY ON (solid PLED), skipping press\n");
            return false;
        }
    }

    logi("my_platform: PC is OFF or SLEEPING -> PRESSING power button!\n");
    gpio_put(POWER_BTN_GPIO, 1);

    btstack_run_loop_set_timer_handler(&btn_off_timer, btn_off_timer_timeout);
    btstack_run_loop_set_timer(&btn_off_timer, POWER_PRESS_MS);
    btn_timer_active = true;
    btstack_run_loop_add_timer(&btn_off_timer);

    return true;
}

// Функция отключения всех подключенных геймпадов.
//
// Вызывается из btstack-таймера. Лок cyw43 async context — рекурсивный
// (pico_cyw43_arch_none использует async_context_threadsafe_background),
// поэтому повторный захват внутри set_global_gamepad_report() безопасен.
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

    // Отсоединяем USB-устройство: так хост (Windows/Linux) реально видит,
    // что геймпад исчез, а не продолжает получать репорты вечно.
    usb_request_attach(false);

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
    int sense_1 = gpio_get(PC_SENSE_GPIO);
    sleep_us(100);
    int sense_2 = gpio_get(PC_SENSE_GPIO);

    bool current_pc_state = (sense_1 == 1 && sense_2 == 1);

    // Детектируем момент ВЫКЛЮЧЕНИЯ ПК (был 1, стал 0).
    // Только при первом измерении НЕ сравниваем: иначе выключенный на момент
    // старта ПК сразу даст ложное событие.
    if (pc_state_initialized && last_pc_state && !current_pc_state) {
        logi("my_platform: Detected PC Shutdown/Sleep event!\n");
        pc_off = true;
        schedule_disconnect_all_controllers();
    }

    // ПК снова включился: возвращаем USB-устройство на шину, чтобы хост
    // снова увидел геймпад. Геймпады при этом переподключаются штатно.
    if (pc_state_initialized && !last_pc_state && current_pc_state) {
        logi("my_platform: Detected PC Power-ON event!\n");
        pc_off = false;
        usb_request_attach(true);
    }

    last_pc_state = current_pc_state;
    pc_state_initialized = true;

    // Перезапускаем таймер проверки.
    // remove+add: таймер снимается process_timers() перед вызовом колбэка,
    // но remove() защищает от случайной повторной регистрации.
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
    pc_off = false;

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

    // Запускаем фоновый мониторинг состояния ПК (каждую секунду)
    btstack_run_loop_set_timer_handler(&pc_status_timer, pc_status_timer_timeout);
    btstack_run_loop_set_timer(&pc_status_timer, PC_CHECK_INTERVAL_MS);
    btstack_run_loop_add_timer(&pc_status_timer);

    logi("BLUEPAD: ready to fill reports\n");
    multicore_fifo_push_blocking(0);
}

static void pico_switch_platform_on_device_connected(uni_hid_device_t* d) {
    logi("my_platform: device connected: %p\n", d);
}

static void pico_switch_platform_on_device_disconnected(uni_hid_device_t* d) {
    logi("my_platform: device disconnected: %p\n", d);
    
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

    // Геймпад готов: гарантированно возвращаем USB-устройство на шину.
    // Если ПК был выключен и геймпадом его разбудили/включили, без этого
    // хост не увидел бы контроллер после пробуждения.
    usb_request_attach(true);

    // Безопасно включаем/разбуживаем ПК при готовности геймпада (false = не принудительно)
    press_power_button_safe(false);

    // НЕ перезаписываем last_pc_state здесь безусловно.
    // Если записать состояние, пока ПК ещё грузится (PLED = 0), мы потеряем
    // фронт 1->0 при последующем выключении и геймпад не отключится.
    // Состоянием управляет только pc_status_timer_timeout().
    // Единственное, что делаем: если состояние ещё ни разу не измерялось —
    // фиксируем текущее как начальное.
    if (!pc_state_initialized) {
        last_pc_state = (gpio_get(PC_SENSE_GPIO) == 1);
        pc_state_initialized = true;
    }

    // Если ПК включён — снимаем флаг «выключен».
    if (last_pc_state) {
        pc_off = false;
    }

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