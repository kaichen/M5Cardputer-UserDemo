/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdint>
#include <string>

/**
 * @brief Live viewfinder for M5Stack Unit CamS3 5MP running its factory firmware.
 *
 * The unit opens an open Wi-Fi hotspot "UnitCamS3-WiFi" (192.168.4.1) and serves JPEG
 * frames at /api/v1/capture. This app joins that hotspot, pulls frames in a background
 * task and draws them scaled to the canvas.
 */
class AppCamera : public mooncake::AppAbility {
public:
    AppCamera();
    ~AppCamera();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class State { Connecting, Streaming, Failed };

    struct FrameBuffer_t {
        uint8_t* data = nullptr;
        size_t len    = 0;
    };

    State _state = State::Connecting;
    std::string _status_message;
    uint32_t _state_time = 0;

    // Frame exchange between fetch task and UI
    FrameBuffer_t _frames[2];
    int _front                 = 0;  // index UI reads from
    volatile bool _new_frame   = false;
    SemaphoreHandle_t _mutex   = nullptr;
    TaskHandle_t _task         = nullptr;
    volatile bool _task_stop   = false;
    volatile bool _task_alive  = false;
    volatile int _fetch_errors = 0;

    // Stats
    uint32_t _frame_count   = 0;
    uint32_t _fps_window_ms = 0;
    uint32_t _fps_frames    = 0;
    float _fps              = 0.0f;
    size_t _last_frame_size = 0;

    bool connect_camera();
    void start_fetch_task();
    void stop_fetch_task();
    static void fetch_task_entry(void* arg);
    void fetch_task();
    bool fetch_frame(FrameBuffer_t& out);
    void set_camera_framesize();

    void render_frame();
    void render_message();
};
