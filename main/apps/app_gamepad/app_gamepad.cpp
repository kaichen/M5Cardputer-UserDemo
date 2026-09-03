/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_gamepad.h"
#include "assets/gamepad_big.h"
#include "assets/gamepad_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <driver/gpio.h>
#include <assets.h>
#include <hal.h>

using namespace mooncake;

/* ------------------------------- Unit config ------------------------------ */
// Grove port pins on Cardputer ADV (same pins as M5.Ex_I2C)
static constexpr gpio_num_t GROVE_PIN_SCL = GPIO_NUM_1;  // Dual Button blue
static constexpr gpio_num_t GROVE_PIN_SDA = GPIO_NUM_2;  // Dual Button red

// Unit Joystick2 (https://github.com/m5stack/M5Unit-Joystick2)
static constexpr uint8_t JOY_I2C_ADDR       = 0x63;
static constexpr uint8_t JOY_REG_ADC_16BITS = 0x00;  // uint16 x, uint16 y, little endian, 0 ~ 65535
static constexpr uint8_t JOY_REG_BUTTON     = 0x20;  // 0 = pressed
static constexpr uint8_t JOY_REG_RGB        = 0x30;  // uint32 0x00RRGGBB
static constexpr uint32_t JOY_I2C_FREQ      = 100000;
static constexpr int JOY_CENTER             = 32768;
static constexpr int JOY_DEAD_ZONE          = 12000;  // Stick must leave center by this much to count as a direction
// Flip these if your stick directions look mirrored on screen
static constexpr bool JOY_INVERT_X = false;
static constexpr bool JOY_INVERT_Y = false;

static constexpr uint32_t UPDATE_INTERVAL_MS = 20;
static constexpr uint32_t PROBE_INTERVAL_MS  = 2000;
static constexpr int I2C_FAIL_LIMIT          = 5;

/* --------------------------------- Colors --------------------------------- */
static constexpr uint32_t COLOR_BODY        = 0xC8C4BC;
static constexpr uint32_t COLOR_BODY_EDGE   = 0x404040;
static constexpr uint32_t COLOR_PLATE       = 0x222222;
static constexpr uint32_t COLOR_DPAD        = 0x6A6A6A;
static constexpr uint32_t COLOR_DPAD_ACTIVE = THEME_COLOR_SYSTEM_BAR;
static constexpr uint32_t COLOR_BTN         = 0xB02020;
static constexpr uint32_t COLOR_BTN_ACTIVE  = 0xFF6060;
static constexpr uint32_t COLOR_PILL        = 0x333333;
static constexpr uint32_t COLOR_TEXT_DIM    = 0x9A9A9A;

AppGamepad::AppGamepad()
{
    setAppInfo().name     = "Gamepad";
    setAppInfo().userData = new AppIcon_t(image_data_gamepad_big, image_data_gamepad_small);
}

AppGamepad::~AppGamepad()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppGamepad::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(&fonts::Font0);
    GetHAL().canvas.setTextSize(1);

    _mode = Mode::None;
    detect_unit();
    render();
}

void AppGamepad::onRunning()
{
    if (GetHAL().millis() - _last_update_time >= UPDATE_INTERVAL_MS) {
        _last_update_time = GetHAL().millis();

        if (_mode == Mode::Joystick2) {
            if (read_joystick()) {
                _i2c_fail_count = 0;
                // Light the stick LED while its button is held
                if (_state.a != _joy_led_on) {
                    _joy_led_on = _state.a;
                    set_joystick_led(_joy_led_on ? 0x00FF3030 : 0x00000000);
                }
            } else if (++_i2c_fail_count >= I2C_FAIL_LIMIT) {
                mclog::tagWarn(getAppInfo().name, "joystick lost, re-detecting");
                detect_unit();
            }
        } else {
            read_dual_button();
            // Hot-plug: retry joystick probe while no button is held
            bool idle = !_state.a && !_state.b;
            if (idle && GetHAL().millis() - _last_probe_time >= PROBE_INTERVAL_MS) {
                detect_unit();
            }
        }

        render();
    }

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppGamepad::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_mode == Mode::Joystick2) {
        set_joystick_led(0x00000000);
        M5.Ex_I2C.release();
    } else if (_mode == Mode::DualButton) {
        stop_dual_button();
    }
    _mode = Mode::None;
}

/* -------------------------------------------------------------------------- */
/*                                 Unit access                                */
/* -------------------------------------------------------------------------- */
void AppGamepad::detect_unit()
{
    if (_mode == Mode::DualButton) {
        stop_dual_button();
    }

    _state           = PadState_t();
    _i2c_fail_count  = 0;
    _joy_led_on      = false;
    _last_probe_time = GetHAL().millis();

    if (probe_joystick()) {
        if (_mode != Mode::Joystick2) {
            mclog::tagInfo(getAppInfo().name, "unit joystick2 found at 0x{:02X}", JOY_I2C_ADDR);
        }
        _mode = Mode::Joystick2;
        return;
    }

    M5.Ex_I2C.release();
    start_dual_button();
    if (_mode != Mode::DualButton) {
        mclog::tagInfo(getAppInfo().name, "no joystick, dual button mode on G{}/G{}", (int)GROVE_PIN_SCL,
                       (int)GROVE_PIN_SDA);
    }
    _mode = Mode::DualButton;
}

bool AppGamepad::probe_joystick()
{
    if (!M5.Ex_I2C.begin()) {
        return false;
    }
    return M5.Ex_I2C.scanID(JOY_I2C_ADDR);
}

// Register read with a STOP between the address write and the data read,
// the most tolerant sequence for STM32-based M5 units.
static bool joy_read_reg(uint8_t reg, uint8_t* buf, size_t len)
{
    auto& i2c = M5.Ex_I2C;
    bool ok   = i2c.start(JOY_I2C_ADDR, false, JOY_I2C_FREQ) && i2c.write(reg);
    ok        = i2c.stop() && ok;
    if (!ok) {
        return false;
    }
    ok = i2c.start(JOY_I2C_ADDR, true, JOY_I2C_FREQ) && i2c.read(buf, len, true);
    ok = i2c.stop() && ok;
    return ok;
}

bool AppGamepad::read_joystick()
{
    uint8_t adc[4];
    if (!joy_read_reg(JOY_REG_ADC_16BITS, adc, sizeof(adc))) {
        return false;
    }
    uint8_t btn;
    if (!joy_read_reg(JOY_REG_BUTTON, &btn, 1)) {
        return false;
    }

    _state.raw_x = adc[0] | (adc[1] << 8);
    _state.raw_y = adc[2] | (adc[3] << 8);

    int dx = _state.raw_x - JOY_CENTER;
    int dy = _state.raw_y - JOY_CENTER;
    if (JOY_INVERT_X) dx = -dx;
    if (JOY_INVERT_Y) dy = -dy;

    _state.left  = dx < -JOY_DEAD_ZONE;
    _state.right = dx > JOY_DEAD_ZONE;
    _state.up    = dy < -JOY_DEAD_ZONE;
    _state.down  = dy > JOY_DEAD_ZONE;
    _state.a     = (btn == 0);
    _state.b     = false;
    return true;
}

void AppGamepad::set_joystick_led(uint32_t rgb)
{
    uint8_t data[4] = {(uint8_t)(rgb & 0xFF), (uint8_t)((rgb >> 8) & 0xFF), (uint8_t)((rgb >> 16) & 0xFF), 0};
    M5.Ex_I2C.writeRegister(JOY_I2C_ADDR, JOY_REG_RGB, data, sizeof(data), JOY_I2C_FREQ);
}

void AppGamepad::start_dual_button()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = (1ULL << GROVE_PIN_SCL) | (1ULL << GROVE_PIN_SDA);
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
}

void AppGamepad::stop_dual_button()
{
    gpio_reset_pin(GROVE_PIN_SCL);
    gpio_reset_pin(GROVE_PIN_SDA);
}

void AppGamepad::read_dual_button()
{
    // Pressed = LOW
    _state.b = gpio_get_level(GROVE_PIN_SCL) == 0;
    _state.a = gpio_get_level(GROVE_PIN_SDA) == 0;
}

/* -------------------------------------------------------------------------- */
/*                                   Render                                   */
/* -------------------------------------------------------------------------- */
// Canvas is 204 x 109. Header text on top, FC-style controller body below.
void AppGamepad::render()
{
    auto& c = GetHAL().canvas;
    c.fillScreen(THEME_COLOR_BG);

    render_header();

    // Controller body
    c.fillSmoothRoundRect(2, 13, 200, 94, 10, COLOR_BODY_EDGE);
    c.fillSmoothRoundRect(6, 17, 192, 86, 7, COLOR_BODY);

    // D-pad plate (left) and A/B plate (right)
    c.fillSmoothRoundRect(12, 24, 72, 72, 5, COLOR_PLATE);
    c.fillSmoothRoundRect(120, 24, 72, 72, 5, COLOR_PLATE);

    render_dpad(48, 60);
    render_ab_buttons();
    render_center(102, 60);

    GetHAL().pushCanvas();
}

void AppGamepad::render_header()
{
    auto& c = GetHAL().canvas;
    c.setTextDatum(top_left);
    c.setTextColor(THEME_COLOR_SYSTEM_BAR);

    if (_mode == Mode::Joystick2) {
        c.drawString("JOYSTICK2", 2, 2);
        c.setTextColor(COLOR_TEXT_DIM);
        c.setTextDatum(top_right);
        c.drawString(fmt::format("X:{:5d} Y:{:5d}", _state.raw_x, _state.raw_y).c_str(), 202, 2);
    } else {
        c.drawString("DUAL BUTTON", 2, 2);
        c.setTextColor(COLOR_TEXT_DIM);
        c.setTextDatum(top_right);
        c.drawString("BLUE=B RED=A", 202, 2);
    }
    c.setTextDatum(top_left);
}

void AppGamepad::render_dpad(int cx, int cy)
{
    auto& c = GetHAL().canvas;
    // Arm thickness 16, reach 28 from center
    constexpr int t = 16;
    constexpr int r = 28;

    auto arm = [&](bool active, int x, int y, int w, int h) {
        c.fillRoundRect(x, y, w, h, 2, active ? COLOR_DPAD_ACTIVE : COLOR_DPAD);
    };
    arm(_state.up, cx - t / 2, cy - r, t, r - t / 2 + 1);
    arm(_state.down, cx - t / 2, cy + t / 2 - 1, t, r - t / 2 + 1);
    arm(_state.left, cx - r, cy - t / 2, r - t / 2 + 1, t);
    arm(_state.right, cx + t / 2 - 1, cy - t / 2, r - t / 2 + 1, t);
    // Center block
    c.fillRect(cx - t / 2, cy - t / 2, t, t, COLOR_DPAD);
    c.fillSmoothCircle(cx, cy, 4, 0x555555);

    // Direction arrows
    uint32_t arrow_idle = 0x3A3A3A;
    auto tri = [&](bool active, int x0, int y0, int x1, int y1, int x2, int y2) {
        c.fillTriangle(x0, y0, x1, y1, x2, y2, active ? COLOR_PLATE : arrow_idle);
    };
    tri(_state.up, cx, cy - r + 4, cx - 4, cy - r + 10, cx + 4, cy - r + 10);
    tri(_state.down, cx, cy + r - 4, cx - 4, cy + r - 10, cx + 4, cy + r - 10);
    tri(_state.left, cx - r + 4, cy, cx - r + 10, cy - 4, cx - r + 10, cy + 4);
    tri(_state.right, cx + r - 4, cy, cx + r - 10, cy - 4, cx + r - 10, cy + 4);
}

void AppGamepad::render_ab_buttons()
{
    auto& c = GetHAL().canvas;
    constexpr int radius = 13;
    const int bx = 143, by = 68;
    const int ax = 173, ay = 52;

    auto button = [&](bool active, int x, int y, const char* label) {
        if (active) {
            c.fillSmoothCircle(x, y, radius + 3, TFT_WHITE);
        }
        c.fillSmoothCircle(x, y, radius, active ? COLOR_BTN_ACTIVE : COLOR_BTN);
        c.setTextDatum(middle_center);
        c.setTextColor(active ? TFT_BLACK : 0xFFC0C0);
        c.drawString(label, x + 1, y + 1);
        c.setTextDatum(top_left);
    };
    button(_state.b, bx, by, "B");
    button(_state.a, ax, ay, "A");
}

void AppGamepad::render_center(int cx, int cy)
{
    auto& c = GetHAL().canvas;

    // Select / Start pills
    c.fillRoundRect(cx - 15, cy + 20, 12, 6, 3, COLOR_PILL);
    c.fillRoundRect(cx + 3, cy + 20, 12, 6, 3, COLOR_PILL);

    if (_mode == Mode::Joystick2) {
        // Analog stick position indicator
        constexpr int ring = 14;
        c.drawCircle(cx, cy - 6, ring, COLOR_PILL);
        c.drawFastHLine(cx - ring, cy - 6, ring * 2 + 1, COLOR_PILL);
        c.drawFastVLine(cx, cy - 6 - ring, ring * 2 + 1, COLOR_PILL);
        int dx = _state.raw_x - JOY_CENTER;
        int dy = _state.raw_y - JOY_CENTER;
        if (JOY_INVERT_X) dx = -dx;
        if (JOY_INVERT_Y) dy = -dy;
        int px = cx + std::clamp(dx * (ring - 3) / JOY_CENTER, -(ring - 3), ring - 3);
        int py = cy - 6 + std::clamp(dy * (ring - 3) / JOY_CENTER, -(ring - 3), ring - 3);
        bool moved = _state.up || _state.down || _state.left || _state.right;
        c.fillSmoothCircle(px, py, 3, moved ? COLOR_DPAD_ACTIVE : COLOR_BODY_EDGE);
    } else {
        c.setTextDatum(middle_center);
        c.setTextColor(COLOR_BODY_EDGE);
        c.drawString("FC", cx, cy - 6);
        c.setTextDatum(top_left);
    }
}
