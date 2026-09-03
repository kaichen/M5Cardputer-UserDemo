/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_camera.h"
#include "assets/camera_big.h"
#include "assets/camera_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <esp_http_client.h>
#include <assets.h>
#include <hal.h>
#include <cstring>

using namespace mooncake;

/* ------------------------------ Camera config ----------------------------- */
// Unit CamS3 5MP factory firmware (https://github.com/m5stack/UnitCamS3-UserDemo, branch unitcams3-5mp)
static constexpr const char* CAM_WIFI_SSID     = "UnitCamS3-WiFi";
static constexpr const char* CAM_WIFI_PASSWORD = "";
static constexpr const char* CAM_URL_CAPTURE   = "http://192.168.4.1/api/v1/capture";
// framesize 6 = QVGA 320x240 (see esp32-camera framesize_t)
static constexpr const char* CAM_URL_FRAMESIZE = "http://192.168.4.1/api/v1/control?var=framesize&val=6";
static constexpr int CAM_IMAGE_WIDTH           = 320;
static constexpr int CAM_IMAGE_HEIGHT          = 240;

static constexpr size_t FRAME_BUFFER_SIZE   = 32 * 1024;  // QVGA JPEG is typically 8 ~ 16 KB
static constexpr int HTTP_TIMEOUT_MS        = 3000;
static constexpr int FETCH_ERROR_LIMIT      = 10;
static constexpr uint32_t RETRY_INTERVAL_MS = 5000;

AppCamera::AppCamera()
{
    setAppInfo().name     = "Camera";
    setAppInfo().userData = new AppIcon_t(image_data_camera_big, image_data_camera_small);
}

AppCamera::~AppCamera()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppCamera::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    _frame_count     = 0;
    _fps             = 0.0f;
    _fps_frames      = 0;
    _fps_window_ms   = GetHAL().millis();
    _last_frame_size = 0;
    _new_frame       = false;
    _front           = 0;

    if (!_mutex) {
        _mutex = xSemaphoreCreateMutex();
    }
    for (auto& f : _frames) {
        if (!f.data) {
            f.data = static_cast<uint8_t*>(malloc(FRAME_BUFFER_SIZE));
        }
        f.len = 0;
    }

    _state          = State::Connecting;
    _status_message = fmt::format("Connecting\n{}", CAM_WIFI_SSID);
    render_message();

    if (connect_camera()) {
        set_camera_framesize();
        start_fetch_task();
        _state      = State::Streaming;
        _state_time = GetHAL().millis();
        _status_message.clear();
        render_message();
    } else {
        _state          = State::Failed;
        _state_time     = GetHAL().millis();
        _status_message = fmt::format("Hotspot not found\n{}\nRetry in 5s", CAM_WIFI_SSID);
        render_message();
    }
}

void AppCamera::onRunning()
{
    switch (_state) {
        case State::Streaming:
            if (_new_frame) {
                render_frame();
            } else if (_fetch_errors >= FETCH_ERROR_LIMIT) {
                mclog::tagWarn(getAppInfo().name, "camera not responding");
                stop_fetch_task();
                GetHAL().wifiDisconnect();
                _state          = State::Failed;
                _state_time     = GetHAL().millis();
                _status_message = "Camera lost\nRetry in 5s";
                render_message();
            }
            break;

        case State::Failed:
            if (GetHAL().millis() - _state_time >= RETRY_INTERVAL_MS) {
                _state          = State::Connecting;
                _status_message = fmt::format("Connecting\n{}", CAM_WIFI_SSID);
                render_message();
                if (connect_camera()) {
                    set_camera_framesize();
                    start_fetch_task();
                    _state = State::Streaming;
                } else {
                    _state          = State::Failed;
                    _status_message = fmt::format("Hotspot not found\n{}\nRetry in 5s", CAM_WIFI_SSID);
                    render_message();
                }
                _state_time = GetHAL().millis();
            }
            break;

        default:
            break;
    }

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppCamera::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    stop_fetch_task();
    GetHAL().wifiDisconnect();

    for (auto& f : _frames) {
        free(f.data);
        f.data = nullptr;
        f.len  = 0;
    }
}

/* -------------------------------------------------------------------------- */
/*                                 Connection                                 */
/* -------------------------------------------------------------------------- */
bool AppCamera::connect_camera()
{
    GetHAL().wifiInit();
    return GetHAL().wifiConnect(CAM_WIFI_SSID, CAM_WIFI_PASSWORD);
}

void AppCamera::set_camera_framesize()
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = CAM_URL_FRAMESIZE;
    cfg.timeout_ms               = HTTP_TIMEOUT_MS;

    auto client = esp_http_client_init(&cfg);
    if (!client) {
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    mclog::tagInfo(getAppInfo().name, "set framesize: {} (http {})", esp_err_to_name(err),
                   esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
}

/* -------------------------------------------------------------------------- */
/*                                 Fetch task                                 */
/* -------------------------------------------------------------------------- */
void AppCamera::start_fetch_task()
{
    if (_task) {
        return;
    }
    _task_stop    = false;
    _task_alive   = true;
    _fetch_errors = 0;
    _new_frame    = false;
    xTaskCreatePinnedToCore(fetch_task_entry, "cam_fetch", 6 * 1024, this, 3, &_task, 0);
}

void AppCamera::stop_fetch_task()
{
    if (!_task) {
        return;
    }
    _task_stop = true;
    while (_task_alive) {
        GetHAL().delay(10);
    }
    _task      = nullptr;
    _new_frame = false;
}

void AppCamera::fetch_task_entry(void* arg)
{
    static_cast<AppCamera*>(arg)->fetch_task();
}

void AppCamera::fetch_task()
{
    while (!_task_stop) {
        // Fill the back buffer while UI may be decoding the front one
        int back = 1 - _front;
        if (fetch_frame(_frames[back])) {
            _fetch_errors = 0;
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _front     = back;
            _new_frame = true;
            xSemaphoreGive(_mutex);
        } else {
            _fetch_errors = _fetch_errors + 1;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        taskYIELD();
    }
    _task_alive = false;
    vTaskDelete(nullptr);
}

bool AppCamera::fetch_frame(FrameBuffer_t& out)
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = CAM_URL_CAPTURE;
    cfg.timeout_ms               = HTTP_TIMEOUT_MS;
    cfg.buffer_size              = 2048;

    auto client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }

    bool ok = false;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            break;
        }
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200) {
            break;
        }
        if (content_length > (int64_t)FRAME_BUFFER_SIZE) {
            mclog::tagWarn(getAppInfo().name, "frame too large: {} bytes", content_length);
            break;
        }

        size_t total = 0;
        while (total < FRAME_BUFFER_SIZE) {
            int n = esp_http_client_read(client, (char*)out.data + total, FRAME_BUFFER_SIZE - total);
            if (n < 0) {
                total = 0;
                break;
            }
            if (n == 0) {
                break;
            }
            total += n;
            if (content_length > 0 && (int64_t)total >= content_length) {
                break;
            }
        }

        // Sanity check: JPEG SOI marker
        if (total > 4 && out.data[0] == 0xFF && out.data[1] == 0xD8) {
            out.len = total;
            ok      = true;
        }
    } while (0);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

/* -------------------------------------------------------------------------- */
/*                                   Render                                   */
/* -------------------------------------------------------------------------- */
void AppCamera::render_frame()
{
    auto& c = GetHAL().canvas;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    auto& frame = _frames[_front];
    _new_frame  = false;

    // Fill canvas width, crop top/bottom (320x240 -> 204x153, keep middle 109 rows)
    const float zoom   = (float)c.width() / CAM_IMAGE_WIDTH;
    const int scaled_h = (int)(CAM_IMAGE_HEIGHT * zoom);
    const int off_y    = (scaled_h - c.height()) / 2;
    bool drawn = c.drawJpg(frame.data, frame.len, 0, 0, c.width(), c.height(), 0, off_y, zoom, zoom);
    _last_frame_size = frame.len;
    xSemaphoreGive(_mutex);

    if (!drawn) {
        mclog::tagWarn(getAppInfo().name, "jpeg decode failed ({} bytes)", _last_frame_size);
        return;
    }

    // Stats
    _frame_count++;
    _fps_frames++;
    uint32_t now = GetHAL().millis();
    if (now - _fps_window_ms >= 1000) {
        _fps           = _fps_frames * 1000.0f / (now - _fps_window_ms);
        _fps_frames    = 0;
        _fps_window_ms = now;
    }

    // Overlay
    c.setFont(&fonts::Font0);
    c.setTextDatum(top_left);
    c.setTextColor(THEME_COLOR_SYSTEM_BAR, TFT_BLACK);
    c.drawString(fmt::format(" {:.1f}fps {:2d}KB ", _fps, (int)(_last_frame_size / 1024)).c_str(), 2, 2);
    c.setTextColor(TFT_WHITE, TFT_BLACK);
    c.drawString(" CamS3 320x240 ", 2, c.height() - 10);
    c.setFont(FONT_REPL);

    GetHAL().pushCanvas();
}

void AppCamera::render_message()
{
    auto& c = GetHAL().canvas;
    c.fillScreen(THEME_COLOR_BG);
    if (_status_message.empty()) {
        GetHAL().pushCanvas();
        return;
    }

    // Center multi-line text
    int line_count = 1;
    for (char ch : _status_message) {
        if (ch == '\n') line_count++;
    }
    int y = (c.height() - line_count * FONT_REPL_HEIGHT) / 2;

    c.setFont(FONT_REPL);
    c.setTextDatum(top_center);
    c.setTextColor(_state == State::Failed ? (uint32_t)0xFF8080 : (uint32_t)0xE6E6E6);

    size_t start = 0;
    while (start <= _status_message.size()) {
        size_t end      = _status_message.find('\n', start);
        std::string row = _status_message.substr(start, end == std::string::npos ? std::string::npos : end - start);
        c.drawString(row.c_str(), c.width() / 2, y);
        y += FONT_REPL_HEIGHT;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    c.setTextDatum(top_left);
    GetHAL().pushCanvas();
}
