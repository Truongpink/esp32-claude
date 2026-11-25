#include "button_manager.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include "audio_manager.h"
#include "display_manager.h"
#include "chatai_xiaozhi_client.h"
#include "diagnostic_manager.h"

namespace chatai::buttons {
namespace {
constexpr char kTag[] = "ChatAI-Buttons";
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr gpio_num_t kTouchPin = GPIO_NUM_43;
constexpr gpio_num_t kVolumeUpPin = GPIO_NUM_40;
constexpr gpio_num_t kVolumeDownPin = GPIO_NUM_39;
constexpr TickType_t kDebounceTicks = pdMS_TO_TICKS(35);
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(10);
constexpr TickType_t kVolumeRepeatTicks = pdMS_TO_TICKS(250);

struct ButtonState {
    gpio_num_t pin;
    bool active_high;
    bool candidate_pressed = false;
    bool stable_pressed = false;
    TickType_t candidate_since = 0;
    TickType_t next_repeat = 0;
};

ButtonState g_boot{kBootPin, false};
ButtonState g_touch{kTouchPin, true};
ButtonState g_volume_up{kVolumeUpPin, false};
ButtonState g_volume_down{kVolumeDownPin, false};
std::atomic<bool> g_initialized{false};

bool ConfigureButton(ButtonState& button) {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << button.pin;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = button.active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    config.pull_down_en = button.active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "GPIO %d setup failed: %s", button.pin, esp_err_to_name(result));
        return false;
    }
    const bool pressed = gpio_get_level(button.pin) == (button.active_high ? 1 : 0);
    button.candidate_pressed = pressed;
    button.stable_pressed = pressed;
    return true;
}

bool UpdateButton(ButtonState& button, TickType_t now) {
    const bool raw_pressed = gpio_get_level(button.pin) == (button.active_high ? 1 : 0);
    if (raw_pressed != button.candidate_pressed) {
        button.candidate_pressed = raw_pressed;
        button.candidate_since = now;
    }
    if (button.candidate_pressed != button.stable_pressed &&
        now - button.candidate_since >= kDebounceTicks) {
        button.stable_pressed = button.candidate_pressed;
        return true;
    }
    return false;
}

void ReportButtonTransition(const char* name, const ButtonState& button) {
    char detail[128]{};
    std::snprintf(detail, sizeof(detail),
                  "button=%s gpio=%d state=%s active_level=%d task_stack_watermark=%u",
                  name, static_cast<int>(button.pin),
                  button.stable_pressed ? "PRESSED" : "RELEASED",
                  button.active_high ? 1 : 0,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    chatai::diagnostics::Add("BUTTON", "INFO", detail);
    if (name != nullptr && std::strcmp(name, "BOOT") == 0) {
        chatai::diagnostics::SetBootContext(
            button.stable_pressed ? "BOOT_PRESS" : "BOOT_RELEASE", detail);
    }
    if (name != nullptr && std::strcmp(name, "PTT") == 0) {
        if (button.stable_pressed) {
            chatai::diagnostics::SetCrashContext("PTT_PRESS", detail);
        } else {
            // Preserve the last button event across a reboot; the next microphone/audio
            // stage will replace this marker when a new listening session starts.
            chatai::diagnostics::SetCrashContext("PTT_RELEASE", detail);
        }
    }
}

void AdjustVolume(int delta) {
    const int volume = std::clamp(chatai::audio::GetOutputVolume() + delta, 0, 100);
    chatai::audio::SetOutputVolume(volume);
    char message[24]{};
    std::snprintf(message, sizeof(message), "Âm lượng %d%%", volume);
    chatai::display::Notify("ÂM LƯỢNG", message, 1200);
    ESP_LOGI(kTag, "Output volume: %d%%", volume);
}

void ButtonTask(void*) {
    while (true) {
        const TickType_t now = xTaskGetTickCount();

        if (UpdateButton(g_boot, now)) {
            ReportButtonTransition("BOOT", g_boot);
            // Xiaozhi BOOT is a click action: process once after the release edge.
            // Wi-Fi provisioning is intentionally not controlled by this button.
            if (!g_boot.stable_pressed) {
                const auto action = chatai::xiaozhi::HandleBootToggle();
                const char* action_name = "IGNORED";
                const char* message = "BOOT không khả dụng ở trạng thái hiện tại";
                switch (action) {
                    case chatai::xiaozhi::BootAction::kStarted:
                        action_name = "START_LISTENING";
                        message = "Bắt đầu nghe liên tục";
                        break;
                    case chatai::xiaozhi::BootAction::kStopped:
                        action_name = "STOP_LISTENING";
                        message = "Đã dừng nghe";
                        break;
                    case chatai::xiaozhi::BootAction::kAborted:
                        action_name = "ABORT_SPEAKING";
                        message = "Đã ngắt phản hồi AI";
                        break;
                    case chatai::xiaozhi::BootAction::kFailed:
                        action_name = "ACTION_FAILED";
                        message = "Không thể thực hiện BOOT";
                        break;
                    case chatai::xiaozhi::BootAction::kIgnored:
                    default:
                        break;
                }
                chatai::display::Notify("XIAOZHI", message);
                char detail[160]{};
                std::snprintf(detail, sizeof(detail),
                    "uptime_ms=%llu action=%s runtime_state=%d",
                    static_cast<unsigned long long>(esp_timer_get_time() / 1000),
                    action_name,
                    static_cast<int>(chatai::xiaozhi::GetRuntimeState()));
                chatai::diagnostics::SetBootContext("BOOT_ACTION", detail);
                chatai::diagnostics::SetCrashContext("BOOT_ACTION_DONE", detail);
                chatai::diagnostics::Add("BOOT_ACTION", "INFO", detail);
            }
        }

        if (UpdateButton(g_touch, now)) {
            ReportButtonTransition("PTT", g_touch);
            chatai::xiaozhi::SetPushToTalk(g_touch.stable_pressed);
            const char* message = !chatai::xiaozhi::IsReady()
                ? "Đang kết nối Xiaozhi"
                : (g_touch.stable_pressed ? "Đang nghe" :
                   (chatai::xiaozhi::IsChatEnabled() ? "Nghe liên tục" : "Đã ngừng nghe"));
            chatai::display::Notify("XIAOZHI", message);
        }

        if (UpdateButton(g_volume_up, now)) {
            ReportButtonTransition("VOLUME_UP", g_volume_up);
            if (g_volume_up.stable_pressed) {
                AdjustVolume(10);
                g_volume_up.next_repeat = now + pdMS_TO_TICKS(450);
            }
        } else if (g_volume_up.stable_pressed &&
                   static_cast<int32_t>(now - g_volume_up.next_repeat) >= 0) {
            AdjustVolume(10);
            g_volume_up.next_repeat = now + kVolumeRepeatTicks;
        }

        if (UpdateButton(g_volume_down, now)) {
            ReportButtonTransition("VOLUME_DOWN", g_volume_down);
            if (g_volume_down.stable_pressed) {
                AdjustVolume(-10);
                g_volume_down.next_repeat = now + pdMS_TO_TICKS(450);
            }
        } else if (g_volume_down.stable_pressed &&
                   static_cast<int32_t>(now - g_volume_down.next_repeat) >= 0) {
            AdjustVolume(-10);
            g_volume_down.next_repeat = now + kVolumeRepeatTicks;
        }

        vTaskDelay(kPollTicks);
    }
}
}  // namespace

esp_err_t Initialize() {
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) return ESP_OK;

    if (!ConfigureButton(g_boot) || !ConfigureButton(g_touch) ||
        !ConfigureButton(g_volume_up) || !ConfigureButton(g_volume_down)) {
        g_initialized.store(false);
        return ESP_FAIL;
    }

    if (xTaskCreate(ButtonTask, "chatai_buttons", 6144, nullptr, 4, nullptr) != pdPASS) {
        g_initialized.store(false);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "Buttons ready: BOOT=GPIO0, PTT=GPIO43, VOL+=GPIO40, VOL-=GPIO39");
    chatai::diagnostics::Add("BUTTON", "READY",
        "mapping=BOOT:GPIO0(active_low),PTT:GPIO43(active_high),VOL_UP:GPIO40(active_low),VOL_DOWN:GPIO39(active_low)");
    return ESP_OK;
}
}  // namespace chatai::buttons
