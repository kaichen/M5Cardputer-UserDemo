/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>

/**
 * @brief FC-style gamepad demo for Grove units on the Grove port (G1 = SCL, G2 = SDA).
 *
 * One unit at a time, auto detected and hot-swappable:
 *  - Unit Joystick2 (I2C 0x63): stick -> D-pad, stick button -> A
 *  - Unit Dual Button (GPIO):   blue (G1) -> B, red (G2) -> A
 *
 * The two units cannot share the port through a Grove hub: Dual Button has a 100 nF
 * debounce capacitor on each signal line, which kills I2C edges.
 */
class AppGamepad : public mooncake::AppAbility {
public:
    AppGamepad();
    ~AppGamepad();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class Mode { None, Joystick2, DualButton };

    struct PadState_t {
        bool up    = false;
        bool down  = false;
        bool left  = false;
        bool right = false;
        bool a     = false;
        bool b     = false;
        // Joystick raw 16-bit ADC (0 ~ 65535), center ~ 32768
        int raw_x = 32768;
        int raw_y = 32768;
    };

    Mode _mode = Mode::None;
    PadState_t _state;
    uint32_t _last_update_time = 0;
    uint32_t _last_probe_time  = 0;
    int _i2c_fail_count        = 0;
    bool _joy_led_on           = false;

    void detect_unit();
    bool probe_joystick();
    bool read_joystick();
    void set_joystick_led(uint32_t rgb);
    void start_dual_button();
    void stop_dual_button();
    void read_dual_button();

    void render();
    void render_header();
    void render_dpad(int cx, int cy);
    void render_ab_buttons();
    void render_center(int cx, int cy);
};
