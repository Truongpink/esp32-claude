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

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "ChatAITPIOSBoard"

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
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
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
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
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
