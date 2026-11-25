#include "status_led_manager.h"

#include <atomic>

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>

#include "diagnostic_manager.h"
#include "chatai_xiaozhi_client.h"
#include "network_manager.h"

namespace chatai::status_led {
namespace {

constexpr char kTag[] = "ChatAI-LED";
constexpr gpio_num_t kDataPin = GPIO_NUM_48;
constexpr uint8_t kBrightness = 8;
constexpr uint8_t kListeningBrightness = 12;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kBootDuration = pdMS_TO_TICKS(1500);

enum class State : uint8_t {
    kUnknown,
    kBoot,
    kWifiConnecting,
    kXiaozhiConnecting,
    kIdle,
    kListening,
    kSpeaking,
    kError,
};

led_strip_handle_t s_strip = nullptr;
std::atomic<bool> s_initialized{false};
TickType_t s_started_tick = 0;

State ResolveState(TickType_t now) {
    if (now - s_started_tick < kBootDuration) return State::kBoot;
    const auto voice_state = chatai::xiaozhi::GetRuntimeState();
    if (voice_state == chatai::xiaozhi::RuntimeState::kError) return State::kError;
    if (voice_state == chatai::xiaozhi::RuntimeState::kSpeaking) return State::kSpeaking;
    if (voice_state == chatai::xiaozhi::RuntimeState::kListening) return State::kListening;
    if (!chatai::network::NetworkManager::IsConnected()) return State::kWifiConnecting;
    if (!chatai::xiaozhi::IsReady() ||
        voice_state == chatai::xiaozhi::RuntimeState::kConnecting ||
        voice_state == chatai::xiaozhi::RuntimeState::kProcessing) {
        return State::kXiaozhiConnecting;
    }
    return State::kIdle;
}

TickType_t BlinkPeriod(State state) {
    switch (state) {
        case State::kBoot: return pdMS_TO_TICKS(100);
        case State::kWifiConnecting: return pdMS_TO_TICKS(500);
        case State::kError: return pdMS_TO_TICKS(250);
        default: return 0;
    }
}

esp_err_t Render(State state, bool visible) {
    if (!visible || state == State::kIdle) {
        return led_strip_clear(s_strip);
    }

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    switch (state) {
        case State::kBoot:
        case State::kWifiConnecting:
        case State::kXiaozhiConnecting:
            blue = kBrightness;
            break;
        case State::kListening:
            red = kListeningBrightness;
            break;
        case State::kSpeaking:
            green = kBrightness;
            break;
        case State::kError:
            red = kListeningBrightness;
            break;
        default:
            return led_strip_clear(s_strip);
    }

    esp_err_t result = led_strip_set_pixel(s_strip, 0, red, green, blue);
    if (result == ESP_OK) result = led_strip_refresh(s_strip);
    return result;
}

void LedTask(void*) {
    State last_state = State::kUnknown;
    bool blink_on = true;
    TickType_t next_toggle = xTaskGetTickCount();
    bool last_visible = false;
    bool update_error_reported = false;

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const State state = ResolveState(now);
        const TickType_t period = BlinkPeriod(state);
        bool should_render = false;

        if (state != last_state) {
            last_state = state;
            blink_on = true;
            next_toggle = now + period;
            should_render = true;
        } else if (period > 0 &&
                   static_cast<int32_t>(now - next_toggle) >= 0) {
            blink_on = !blink_on;
            next_toggle = now + period;
            should_render = true;
        }

        const bool visible = period == 0 || blink_on;
        if (visible != last_visible) should_render = true;

        if (should_render) {
            const esp_err_t result = Render(state, visible);
            if (result != ESP_OK && !update_error_reported) {
                update_error_reported = true;
                ESP_LOGW(kTag, "LED update failed: %s", esp_err_to_name(result));
                chatai::diagnostics::Add("LED_UPDATE", "FAIL", esp_err_to_name(result));
            } else if (result == ESP_OK) {
                update_error_reported = false;
            }
            last_visible = visible;
        }

        vTaskDelay(kPollTicks);
    }
}

}  // namespace

esp_err_t Initialize() {
    bool expected = false;
    if (!s_initialized.compare_exchange_strong(expected, true)) return ESP_OK;

    led_strip_config_t strip_config{};
    strip_config.strip_gpio_num = kDataPin;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config{};
    rmt_config.resolution_hz = 10 * 1000 * 1000;

    esp_err_t result = led_strip_new_rmt_device(
        &strip_config, &rmt_config, &s_strip);
    if (result != ESP_OK) {
        s_initialized.store(false);
        ESP_LOGW(kTag, "WS2812 GPIO48 initialization failed: %s", esp_err_to_name(result));
        chatai::diagnostics::Add("LED_INIT", "FAIL", esp_err_to_name(result));
        return result;
    }

    result = led_strip_clear(s_strip);
    if (result != ESP_OK) {
        led_strip_del(s_strip);
        s_strip = nullptr;
        s_initialized.store(false);
        chatai::diagnostics::Add("LED_INIT", "FAIL", esp_err_to_name(result));
        return result;
    }

    s_started_tick = xTaskGetTickCount();
    if (xTaskCreate(LedTask, "chatai_led", 2048, nullptr, 2, nullptr) != pdPASS) {
        led_strip_del(s_strip);
        s_strip = nullptr;
        s_initialized.store(false);
        chatai::diagnostics::Add("LED_INIT", "FAIL", "could not create LED status task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "WS2812 status LED initialized on GPIO48");
    chatai::diagnostics::Add("LED_INIT", "OK", "WS2812 RGB, GPIO48, one pixel, GRB/RMT");
    return ESP_OK;
}

}  // namespace chatai::status_led
