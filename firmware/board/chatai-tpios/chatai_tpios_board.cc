#include "wifi_board.h"
#include "web_admin_server.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "settings.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_err.h>
#include <utility>
#include <memory>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <mdns.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "ChatAITPIOSBoard"

class ChatAIReferenceCodec final : public NoAudioCodecSimplex {
public:
    ChatAIReferenceCodec(int input_sample_rate, int output_sample_rate,
                         gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                         gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
        : NoAudioCodecSimplex(input_sample_rate, output_sample_rate, spk_bclk, spk_ws,
                              spk_dout, mic_sck, mic_ws, mic_din) {
        esp_ae_rate_cvt_cfg_t config = {
            .src_rate = static_cast<uint32_t>(output_sample_rate),
            .dest_rate = static_cast<uint32_t>(input_sample_rate),
            .channel = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        esp_ae_rate_cvt_open(&config, &reference_resampler_);
        if (reference_resampler_ != nullptr) {
            input_reference_ = true;
            input_channels_ = 2;  // Interleaved microphone (M) + speaker reference (R).
            ESP_LOGI(TAG, "Software speaker reference enabled for ESP-SR AEC");
        } else {
            ESP_LOGW(TAG, "Speaker reference resampler unavailable; AEC remains disabled");
        }
    }

    ~ChatAIReferenceCodec() override {
        reference_capture_enabled_.store(false);
        std::lock_guard<std::mutex> lock(reference_resampler_mutex_);
        if (reference_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(reference_resampler_);
            reference_resampler_ = nullptr;
        }
    }

protected:
    int Read(int16_t* dest, int samples) override {
        if (!input_reference_ || samples <= 0 || (samples & 1) != 0) {
            return NoAudioCodec::Read(dest, samples);
        }

        const int mic_samples = samples / 2;
        std::vector<int16_t> microphone(mic_samples, 0);
        const int received = NoAudioCodec::Read(microphone.data(), mic_samples);
        if (received <= 0) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(reference_mutex_);
        for (int i = 0; i < mic_samples; ++i) {
            const bool has_microphone_sample = i < received;
            dest[i * 2] = has_microphone_sample ? microphone[i] : 0;
            dest[i * 2 + 1] = 0;
            if (has_microphone_sample && reference_size_ > 0) {
                dest[i * 2 + 1] = reference_buffer_[reference_head_];
                reference_head_ = (reference_head_ + 1) % kReferenceBufferCapacity;
                --reference_size_;
            }
        }
        // Keep the AFE frame size fixed; pad a short I2S read with silence.
        return samples;
    }

    int Write(const int16_t* data, int samples) override {
        if (reference_resampler_ != nullptr && samples > 0 &&
            reference_capture_enabled_.load()) {
            std::lock_guard<std::mutex> lock(reference_resampler_mutex_);
            if (reference_capture_enabled_.load()) {
                std::vector<int16_t> scaled(samples);
                const int32_t volume_factor = static_cast<int32_t>(
                    std::pow(static_cast<double>(output_volume_) / 100.0, 2.0) * 65536.0);
                for (int i = 0; i < samples; ++i) {
                    const int64_t value = (static_cast<int64_t>(data[i]) * volume_factor) >> 16;
                    scaled[i] = static_cast<int16_t>(std::clamp<int64_t>(
                        value, std::numeric_limits<int16_t>::min(),
                        std::numeric_limits<int16_t>::max()));
                }

                uint32_t max_output_samples = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(
                    reference_resampler_, static_cast<uint32_t>(samples), &max_output_samples);
                if (max_output_samples > 0) {
                    std::vector<int16_t> reference(max_output_samples);
                    uint32_t actual_output_samples = max_output_samples;
                    esp_ae_rate_cvt_process(
                        reference_resampler_, reinterpret_cast<esp_ae_sample_t*>(scaled.data()),
                        static_cast<uint32_t>(samples),
                        reinterpret_cast<esp_ae_sample_t*>(reference.data()),
                        &actual_output_samples);
                    if (actual_output_samples > 0) {
                        PushReference(reference.data(), actual_output_samples);
                    }
                }
            }
        }
        return NoAudioCodec::Write(data, samples);
    }

    void EnableInput(bool enable) override {
        reference_capture_enabled_.store(false);
        if (!enable) {
            NoAudioCodec::EnableInput(false);
        }
        {
            std::lock_guard<std::mutex> resampler_lock(reference_resampler_mutex_);
            if (reference_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(reference_resampler_);
            }
        }
        {
            std::lock_guard<std::mutex> queue_lock(reference_mutex_);
            reference_head_ = 0;
            reference_size_ = 0;
        }
        if (enable) {
            NoAudioCodec::EnableInput(true);
            reference_capture_enabled_.store(reference_resampler_ != nullptr);
        }
    }

private:
    static constexpr size_t kReferenceBufferCapacity = 8192;  // 512 ms at 16 kHz.
    std::array<int16_t, kReferenceBufferCapacity> reference_buffer_{};
    size_t reference_head_ = 0;
    size_t reference_size_ = 0;
    std::mutex reference_mutex_;
    std::mutex reference_resampler_mutex_;
    esp_ae_rate_cvt_handle_t reference_resampler_ = nullptr;
    std::atomic<bool> reference_capture_enabled_{false};

    void PushReference(const int16_t* samples, size_t count) {
        std::lock_guard<std::mutex> lock(reference_mutex_);
        if (count > kReferenceBufferCapacity) {
            samples += count - kReferenceBufferCapacity;
            count = kReferenceBufferCapacity;
        }
        if (reference_size_ + count > kReferenceBufferCapacity) {
            // A stalled input task invalidates the old timing; restart from the newest audio.
            reference_head_ = 0;
            reference_size_ = 0;
        }
        for (size_t i = 0; i < count; ++i) {
            const size_t tail = (reference_head_ + reference_size_) % kReferenceBufferCapacity;
            reference_buffer_[tail] = samples[i];
            ++reference_size_;
        }
    }
};

class ChatAITPIOSBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    std::unique_ptr<Display> display_ = std::make_unique<NoDisplay>();
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeSpeakerAmplifier() {
        ESP_ERROR_CHECK(gpio_reset_pin(SPEAKER_AMP_ENABLE_GPIO));
        ESP_ERROR_CHECK(gpio_set_direction(SPEAKER_AMP_ENABLE_GPIO, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(SPEAKER_AMP_ENABLE_GPIO, 1));
        ESP_LOGI(TAG, "Speaker amplifier enabled on GPIO%d", SPEAKER_AMP_ENABLE_GPIO);
    }

    bool InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        const esp_err_t err = i2c_new_master_bus(&bus_config, &display_i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus initialization failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    void InitializeSsd1306Display() {
        if (display_i2c_bus_ == nullptr) {
            ESP_LOGW(TAG, "OLED initialization skipped: I2C bus is unavailable");
            return;
        }
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        esp_err_t err = esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED panel I2C setup failed: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        err = esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_);
#else
        err = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED driver creation failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        err = esp_lcd_panel_reset(panel_);
        if (err == ESP_OK) err = esp_lcd_panel_init(panel_);
        if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel_, false);
        if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED initialization failed: %s; continuing without display", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "OLED initialized: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
        display_ = std::make_unique<OledDisplay>(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        // Hold BOOT for 2 seconds to reopen Wi-Fi setup after normal startup.
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long-press: entering Wi-Fi configuration mode");
            EnterWifiConfigMode();
        });
        boot_button_.OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            Settings settings("chatai", true);
            settings.SetInt("volume", volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            Settings settings("chatai", true);
            settings.SetInt("volume", 100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            Settings settings("chatai", true);
            settings.SetInt("volume", volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            Settings settings("chatai", true);
            settings.SetInt("volume", 0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
    }

public:
    ChatAITPIOSBoard() :
        boot_button_(BOOT_BUTTON_GPIO, false, 2000),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        WebAdminServer::InitializeDiagnostics();
        InitializeSpeakerAmplifier();
        if (InitializeDisplayI2c()) {
            InitializeSsd1306Display();
        }
        InitializeButtons();
        InitializeTools();
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                if (event == NetworkEvent::Connected) {
                    static bool mdns_ready = false;
                    if (!mdns_ready) {
                        esp_err_t err = mdns_init();
                        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                            err = mdns_hostname_set("chatai-tpios");
                            if (err == ESP_OK) {
                                const esp_err_t service_err =
                                    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
                                if (service_err != ESP_OK) {
                                    ESP_LOGW(TAG, "Could not advertise Web Admin service: %s",
                                             esp_err_to_name(service_err));
                                }
                                mdns_ready = true;
                                ESP_LOGI(TAG, "Web Admin hostname: http://chatai-tpios.local");
                            } else {
                                ESP_LOGW(TAG, "Could not set mDNS hostname: %s", esp_err_to_name(err));
                            }
                        } else {
                            ESP_LOGW(TAG, "Could not initialize mDNS: %s", esp_err_to_name(err));
                        }
                    }
                    WebAdminServer::Start();
                } else if (event == NetworkEvent::Disconnected ||
                           event == NetworkEvent::WifiConfigModeEnter) {
                    WebAdminServer::Stop();
                }
                if (callback) callback(event, data);
            }
        );
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static ChatAIReferenceCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        static const bool volume_loaded = []() {
            Settings settings("chatai", false);
            audio_codec.SetOutputVolume(settings.GetInt("volume", 70));
            return true;
        }();
        (void)volume_loaded;
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_.get();
    }
};

DECLARE_BOARD(ChatAITPIOSBoard);
