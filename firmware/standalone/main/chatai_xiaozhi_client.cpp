#include "chatai_xiaozhi_client.h"

#include "application.h"
#include "device_state.h"
#include "protocol.h"
#include "network_manager.h"

#include <atomic>
#include <esp_log.h>

namespace chatai::xiaozhi {

extern std::atomic<bool> g_application_initialized;

namespace {
constexpr char kTag[] = "ChatAI-Xiaozhi";
std::atomic<bool> g_init_attempted{false};
std::atomic<bool> g_continuous_chat_enabled{false};
std::atomic<bool> g_push_to_talk{false};
std::atomic<bool> g_stop_after_speech{false};

void FinishDeferredStop(Application& app) {
    if (g_push_to_talk.load() || !g_stop_after_speech.load() ||
        app.GetDeviceState() != kDeviceStateListening) {
        return;
    }
    if (g_stop_after_speech.exchange(false)) {
        g_continuous_chat_enabled.store(false);
        app.ToggleChatState();
    }
}

bool IsConversationState(DeviceState state) {
    return state == kDeviceStateConnecting || state == kDeviceStateListening ||
           state == kDeviceStateSpeaking || state == kDeviceStateNotifying;
}

void SynchronizeContinuousChatState(Application& app) {
    if (!g_stop_after_speech.load() &&
        (!chatai::network::NetworkManager::IsConnected() ||
         !IsConversationState(app.GetDeviceState()))) {
        g_continuous_chat_enabled.store(false);
    }
}
}  // namespace

esp_err_t Initialize() {
    g_init_attempted.store(true);
    g_continuous_chat_enabled.store(false);
    g_push_to_talk.store(false);
    g_stop_after_speech.store(false);

    Application::GetInstance().Initialize();
    if (!g_application_initialized.load()) {
        ESP_LOGE(kTag, "Upstream Application initialization did not complete");
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "Xiaozhi Application initialized");
    return ESP_OK;
}

void Run() {
    if (g_application_initialized.load()) {
        Application::GetInstance().Run();
    }
}

bool IsReady() {
    if (!g_application_initialized.load() ||
        !chatai::network::NetworkManager::IsConnected()) return false;
    const auto state = Application::GetInstance().GetDeviceState();
    return state == kDeviceStateIdle || state == kDeviceStateListening ||
           state == kDeviceStateSpeaking || state == kDeviceStateNotifying;
}

bool StartConversation() {
    if (!IsReady()) return false;
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle) {
        g_continuous_chat_enabled.store(true);
        g_stop_after_speech.store(false);
        app.ToggleChatState();
        return true;
    }
    if (state == kDeviceStateListening) {
        g_continuous_chat_enabled.store(true);
        g_stop_after_speech.store(false);
        return true;
    }
    return false;
}

void SetChatEnabled(bool enabled) {
    if (!g_application_initialized.load() || (enabled && !IsReady())) return;
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();

    if (enabled) {
        g_continuous_chat_enabled.store(true);
        g_stop_after_speech.store(false);
        if (state == kDeviceStateIdle) {
            app.ToggleChatState();
        }
        return;
    }

    g_continuous_chat_enabled.store(false);
    if (g_push_to_talk.load()) return;

    if (state == kDeviceStateListening) {
        app.ToggleChatState();
    } else if (state == kDeviceStateSpeaking) {
        g_stop_after_speech.store(true);
        app.AbortSpeaking(kAbortReasonNone);
    }
}

bool IsChatEnabled() {
    if (g_application_initialized.load()) {
        auto& app = Application::GetInstance();
        FinishDeferredStop(app);
        SynchronizeContinuousChatState(app);
    }
    return g_continuous_chat_enabled.load();
}

void SetPushToTalk(bool pressed) {
    if (!g_application_initialized.load()) return;
    auto& app = Application::GetInstance();
    SynchronizeContinuousChatState(app);
    if (pressed && !IsReady()) return;
    const bool previous = g_push_to_talk.exchange(pressed);
    if (previous == pressed || g_continuous_chat_enabled.load()) return;

    if (pressed) {
        app.StartListening();
    } else {
        app.StopListening();
    }
}

bool IsListening() {
    if (g_application_initialized.load()) {
        auto& app = Application::GetInstance();
        FinishDeferredStop(app);
        SynchronizeContinuousChatState(app);
    }
    return g_continuous_chat_enabled.load() || g_push_to_talk.load();
}

RuntimeState GetDisplayRuntimeState() {
    if (!g_application_initialized.load()) {
        return g_init_attempted.load() ? RuntimeState::kError : RuntimeState::kConnecting;
    }

    const auto device_state = Application::GetInstance().GetDeviceState();
    if (device_state == kDeviceStateFatalError) return RuntimeState::kError;
    if (!chatai::network::NetworkManager::IsConnected()) return RuntimeState::kConnecting;

    switch (device_state) {
        case kDeviceStateIdle:
            return RuntimeState::kWaiting;
        case kDeviceStateListening:
            return RuntimeState::kListening;
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            return RuntimeState::kSpeaking;
        case kDeviceStateAudioTesting:
            return RuntimeState::kProcessing;
        case kDeviceStateFatalError:
            return RuntimeState::kError;
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateConnecting:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading:
        case kDeviceStateUnknown:
        default:
            return RuntimeState::kConnecting;
    }
}

RuntimeState GetRuntimeState() {
    if (g_application_initialized.load()) {
        auto& app = Application::GetInstance();
        FinishDeferredStop(app);
        SynchronizeContinuousChatState(app);
    }
    return GetDisplayRuntimeState();
}

AudioUplinkDiagnostics GetAudioUplinkDiagnostics() {
    AudioUplinkDiagnostics result{};
    if (!g_application_initialized.load()) return result;

    const auto stats = Application::GetInstance().GetAudioService().GetUplinkDiagnostics();
    result.available = true;
    result.pcm_frames_queued = stats.pcm_frames_queued;
    result.opus_frames_encoded = stats.opus_frames_encoded;
    result.encode_failures = stats.encode_failures;
    result.encode_queue_drops = stats.encode_queue_drops;
    result.send_queue_drops = stats.send_queue_drops;
    return result;
}

BootAction HandleBootToggle() {
    if (!g_application_initialized.load()) return BootAction::kFailed;
    if (g_push_to_talk.load()) return BootAction::kIgnored;

    auto& app = Application::GetInstance();
    switch (GetRuntimeState()) {
        case RuntimeState::kWaiting:
            return StartConversation() ? BootAction::kStarted : BootAction::kFailed;
        case RuntimeState::kListening:
            if (!g_continuous_chat_enabled.load()) return BootAction::kIgnored;
            SetChatEnabled(false);
            return BootAction::kStopped;
        case RuntimeState::kSpeaking:
            app.AbortSpeaking(kAbortReasonNone);
            return BootAction::kAborted;
        case RuntimeState::kConnecting:
        case RuntimeState::kProcessing:
        case RuntimeState::kError:
        default:
            return BootAction::kIgnored;
    }
}

}  // namespace chatai::xiaozhi
