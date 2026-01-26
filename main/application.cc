#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "esp_sleep.h"
#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include "wifi_board.h"
#include <ssid_manager.h>
#include "wifi_station.h"
#include <esp_netif.h>
#include "esp_wifi.h"
#include "esp32_rc522.h"
#include "esp_task_wdt.h"
#include "esp_tts.h"
#include "bat_monitor.h"
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <driver/ledc.h>

#define TAG "Application"

extern bool NotResumePlayback;
static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    esp_timer_create_args_t clock_Offlinetimer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->Offline_ticks_++;
            ESP_LOGI(TAG, "Offline tick: %d", app->Offline_ticks_);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_Offline_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_Offlinetimer_args, &clock_Offlinetimer_handle_);

}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

// 全局唤醒计时（ms），0 表示未计时
static std::atomic<int64_t> s_wake_start_ms{0};

static inline void StartWakeTimerInternal() {
    int64_t now = esp_timer_get_time() / 1000;
    s_wake_start_ms.store(now, std::memory_order_release);
    ESP_LOGE(TAG, "Wake timer started ");
}

static inline int64_t ConsumeWakeStartMs() {
    return s_wake_start_ms.exchange(0, std::memory_order_acq_rel);
}
int64_t Application::GetAndClearWakeElapsedMs() {
    // 取出并清零起始时间（0 表示无计时）
    int64_t start = s_wake_start_ms.exchange(0, std::memory_order_acq_rel);
    if (start == 0) return 0;
    int64_t now = esp_timer_get_time() / 1000;
    int64_t elapsed = now - start;
    return elapsed > 0 ? elapsed : 0;
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    //对asset分区进行映射，返回一个Assets实例
    auto& assets = Assets::GetInstance();

    //映射失败
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    //准备写入nvs
    Settings settings("assets", true);

    // Check if there is a new assets need to be downloaded
    //从nvs里读取下载网址
    std::string download_url = settings.GetString("download_url");

    //下载网址非空
    if (!download_url.empty()) {
        //删除nvs里的下载网址，防止重复下载
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        //等待音效结束 -》OGG_UPGRADE
        vTaskDelay(pdMS_TO_TICKS(3000));

        SetDeviceState(kDeviceStateUpgrading);
        //进入非省电模式
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        //取消之前挂载的asset分区并擦除，然后把新下载的asset资源映射进去
        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        //进入省电模式
        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    //把asset分区里的资源加载到内存直接使用，该函数就是一个纯解析CJSON的函数
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        //判断是否有新版本存在
        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 1; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 1);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}


void Application::ShowBatteryLevel(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    percent = (percent / 10) * 10; // 四舍五入到最近的10的倍数
    // 显示文字
    char buf[64];
    snprintf(buf, sizeof(buf), "当前电量：%d%%", percent);
    Alert("电量", buf, "battery", std::string_view());

    // 单数字音效映射（0-9）
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1},
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    auto PlayDigit = [&](char d) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [d](const digit_sound& ds) { return ds.digit == d; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    };

    // 先播放“当前电量”提示音（如果存在）
    audio_service_.PlaySound(Lang::Sounds::OGG_BATTERYLEVEL);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 对于 20..100 范围，优先使用整十位音频（你已支持 20..100）
    if (percent >= 20 && percent <= 100) {
        int tens = (percent / 10) * 10; // 20,30,...,100
        int ones = percent % 10;

        // 播放十位语音（如果存在对应素材）
        switch (tens) {
            case 20: audio_service_.PlaySound(Lang::Sounds::OGG_20); break;
            case 30: audio_service_.PlaySound(Lang::Sounds::OGG_30); break;
            case 40: audio_service_.PlaySound(Lang::Sounds::OGG_40); break;
            case 50: audio_service_.PlaySound(Lang::Sounds::OGG_50); break;
            case 60: audio_service_.PlaySound(Lang::Sounds::OGG_60); break;
            case 70: audio_service_.PlaySound(Lang::Sounds::OGG_70); break;
            case 80: audio_service_.PlaySound(Lang::Sounds::OGG_80); break;
            case 90: audio_service_.PlaySound(Lang::Sounds::OGG_90); break;
            case 100: audio_service_.PlaySound(Lang::Sounds::OGG_100); break;
            default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(140));

        // 100 属于整百，不再播个位
        if (tens == 100) return;

        // 播放个位（非 0 时）
        if (ones != 0) {
            PlayDigit(char('0' + ones));
        }
        return;
    }

    // 回退：逐位播放（用于 0..19 或者没有十位素材的情况）
    std::string s = std::to_string(percent);
    for (const auto& ch : s) {
        PlayDigit(ch);
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                auto &board = Board::GetInstance();
                auto music = board.GetMusic();
                if(music->ReturnMode() == true)
                {
                    wake_word_detected_ = true;
                }
                SetDeviceState(kDeviceStateConnecting);

                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::GetSwitchState(){
    auto ledmode = gpio_get_level(LEDMODE_GPIO);
    auto normalmode = gpio_get_level(NORMALMODE_GPIO);
    ESP_LOGI(TAG, "ledmode: %d, normalmode: %d", ledmode, normalmode);
    // device_function_ = Function_AIAssistant;
    if(ledmode == 0 && normalmode ==1) {
        device_function_ = Function_Light;
    } else if(ledmode ==1 && normalmode ==0) {
        device_function_ = Function_AIAssistant;
    }
    // device_function_ = Function_Light;
}

bool IsWifiConfigMode() {
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    Settings settings("wifi", true);
    return settings.GetInt("force_ap") == 1 || ssid_list.empty();
}

void Application::Start() {
    
    // bool en = IsWifiConfigMode();
    //在这一步就已经调用了board的构造函数来进行关于板级硬件的初始化了
    auto& board = Board::GetInstance();

    //获取设备功能
    GetSwitchState();
    if(device_function_ == Function_Light) {
        ESP_LOGI(TAG, "Switch state: Light");
        board.GetBacklight()->RestoreBrightness(true);
        return ;
    }
    else if(device_function_ == Function_AIAssistant) {
        board.GetBacklight()->RestoreBrightness(false);
        ESP_LOGI(TAG, "Switch state: AIAssistant");
    }
    else {
        ESP_LOGI(TAG, "Switch state: Unknown mode, proceed with normal mode");
    }
    SetDeviceState(kDeviceStateStarting);

    Settings settings("device", true);
    device_Role = (Role)settings.GetInt("device_role");

    /* Setup the display */
    //申请显示器资源
    auto display = board.GetDisplay();

    // Print board name/version info
    //这里调用的是display的派生类LcdDisplay的SetChatMessage函数
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());
        ESP_LOGI(TAG, "关闭RFID");

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    //     char ret = PcdHardPowerDown();
    // if (ret == MI_OK) {
    //     ESP_LOGW(TAG, "PcdHardPowerDown 成功");
    // }
    // else {
    //     ESP_LOGE(TAG, "%x", ret);
    //     ESP_LOGE(TAG, "PcdHardPowerDown 失败");
    // }

    audio_service_.Initialize(codec);
    audio_service_.Start();
//     codec->Shutdown(); // 关闭 codec 输出
//     board.Deinitialize();
//    esp_deep_sleep_start();
 

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 5
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 5, &main_event_loop_task_handle_);

    xTaskCreate([](void* arg) { 
        ((Application*)arg)->RFID_TASK();
        vTaskDelete(NULL);
    }, "rfid_task", 2048 * 4, this, 2, &rfid_task_handle_);

    /* Start the clock timer to update the status bar */
    //该定时器任务会在定时结束对应的event_group_设置MAIN_EVENT_CLOCK_TICK位
    //然后触发main_event_loop函数中的对应处理
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();
    // codec->Shutdown();
    
    // vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    //检查云端是否有新的asset资源需要下载
    //检验失败的话就无法使用外部主题包，设备自动降级到内置资源，功能照常使用
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    //ota，先向服务器发送本地的版本，然后服务器会返回URL
    //如果服务器的版本优于本地，就直接升级，或者有强制升级标志位也升级
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
        ESP_LOGW(TAG, " OTA config, using MQTT");
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
        ESP_LOGW(TAG, " OTA config, using Websocket");
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    //绑定一些回调函数
    protocol_->OnConnected([this]() {
        DismissAlert();
        esp_timer_stop(clock_Offlinetimer_handle_);
        Offline_ticks_ = 0;
        if(this->GetDeviceState() == kDeviceStateWifiConfiguring)
        this->SetDeviceState(kDeviceStateIdle);
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        ESP_LOGE(TAG, "Network error: %s", message.c_str());
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    //解析云端发来的Json数据
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        //tts 文本转语音
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } 
        //stt 语音转文本
        else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } 
        //llm，大语言模型
        else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);
    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
    auto music = board.GetMusic();
    if(music) {
        music->ScanAndLoadMusic();
        music->ScanAndLoadStory();
    }
    esp_reset_reason_t reason = esp_reset_reason();

    switch (reason) {
    case ESP_RST_POWERON:
    ESP_LOGW(TAG,"复位原因: 上电复位\n");
    break;
    case ESP_RST_SW:
    ESP_LOGW(TAG,"复位原因: 软件复位\n");
    break;
    case ESP_RST_PANIC:
    ESP_LOGW(TAG,"复位原因: 异常或崩溃复位\n");
    break;
    case ESP_RST_INT_WDT:
    ESP_LOGW(TAG,"复位原因: 中断看门狗复位\n");
    break;
    case ESP_RST_TASK_WDT:
    ESP_LOGW(TAG,"复位原因: 任务看门狗复位\n");
    break;
    case ESP_RST_DEEPSLEEP:
    ESP_LOGW(TAG,"复位原因: 深度睡眠唤醒\n");
    break;
    default:
    break;
    }
    #if !my
    ShowBatteryLevel(Board::GetInstance().GetBatteryLevel());
    vTaskDelay(pdMS_TO_TICKS(3000));
    #endif

    // esp_deep_sleep_start();
    last_device_Role = device_Role;
    // SetAecMode(kAecOff);
    ESP_LOGI(TAG, "Loaded device role from NVS: %d", device_Role);
    std::string msg = "向用户问好";
    SendMessage(msg);
    
    vTaskDelay(pdMS_TO_TICKS(10000));

}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::EnterDeepSleep() {
    ESP_LOGI(TAG, "=============准备进入深度睡眠===============");
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    if(music->ReturnMode() == true)
    {
        ESP_LOGI(TAG, "退出音乐模式");
        while(music->IsPlaying() != true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));   
        }
        music->StopStreaming();
    }


    ESP_LOGI(TAG, "关闭RFID");
    char ret = PcdHardPowerDown();
    if (ret == MI_OK) {
        ESP_LOGW(TAG, "PcdHardPowerDown 成功");
    }
    else {
        ESP_LOGE(TAG, "%x", ret);
        ESP_LOGE(TAG, "PcdHardPowerDown 失败");
    }
    ESP_LOGI(TAG,"停止ADC电量监测");
    bat_monitor_destroy(battery_handle);

    
    // 停止音频服务并关闭 codec 输出
    ESP_LOGI(TAG, "停止音频服务并关闭音频输出");


    audio_service_.Stop();
    protocol_->Deinit(); // 销毁协议实例以释放资源
    auto codec = board.GetAudioCodec();
    codec->Shutdown(); // 关闭 codec 输出
    board.Deinitialize();//关闭外设

    board.StopWifiTimer();
    // 停止定时器以降低唤醒前的活动
    ESP_LOGI(TAG, "停止定时器");
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
    }
    if (clock_Offlinetimer_handle_ != nullptr) {
        esp_timer_stop(clock_Offlinetimer_handle_);
    }

    ESP_LOGI(TAG, "关闭WiFi");
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();


    ESP_LOGI(TAG, "关闭LED");
    gpio_set_level(GPIO_NUM_6, 0);  // 输出低电平关闭LED（假设低电平点亮）
    gpio_deep_sleep_hold_dis();  // 禁用深度睡眠保持

    ESP_LOGI(TAG,"关闭夜灯");
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    // 对于背光（高电平点亮）：输出低电平
    gpio_set_level(GPIO_NUM_42, 0);


    vTaskDelay(pdMS_TO_TICKS(100)); // 确保所有操作完成
    //ext0 仅支持 RTC IO（例如 GPIO0），若 wake_gpio 非 RTC 引脚可能无法生效
    esp_err_t rc = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_ext0_wakeup 返回 %d", rc);
    }
    
    ESP_LOGI(TAG, "=============进入深度睡眠===============");
    esp_deep_sleep_start();
}

uint8_t read_write_data[16]={0};//读写数据缓存
uint8_t card_KEY[6] ={0xff,0xff,0xff,0xff,0xff,0xff};//默认密码
uint8_t ucArray_ID [ 4 ];
uint8_t ucStatusReturn;    //返回状态
uint8_t data[16] = {0};  // 16字节缓冲区，全部初始化为0
uint8_t LastUID[4] = {0}; // 上次读取的卡片ID
void Application::RFID_TASK()
{

    auto &board = Board::GetInstance();
    auto led = board.GetLed();
    while(1)
    {
        #if !my
            if ( ( ucStatusReturn = PcdRequest ( PICC_REQALL, ucArray_ID ) ) != MI_OK )
            {
                ucStatusReturn = PcdRequest ( PICC_REQALL, ucArray_ID );
            }
            
            if ( ucStatusReturn == MI_OK  )
            {

            /* 防冲突操作，被选中的卡片序列传入数组ucArray_ID中 */
            if ( PcdAnticoll ( ucArray_ID ) == MI_OK )
            {
                ESP_LOGW(TAG,"Card Detected: %02X %02X %02X %02X", ucArray_ID[0], ucArray_ID[1], ucArray_ID[2], ucArray_ID[3]);
                // if(LastUID[0] == ucArray_ID[0] &&
                //    LastUID[1] == ucArray_ID[1] &&
                //    LastUID[2] == ucArray_ID[2] &&
                //    LastUID[3] == ucArray_ID[3])
                // {
                //     vTaskDelay( pdMS_TO_TICKS(500) );
                //     continue; // 如果和上次读取的卡片ID相同，则跳过后续操作
                // }
                // ESP_LOGW(TAG,"Card Detected: %02X %02X %02X %02X", ucArray_ID[0], ucArray_ID[1], ucArray_ID[2], ucArray_ID[3]);
                // PcdSelect(ucArray_ID);              // 选中卡
                // // 2. 认证块1（第一个数据块）
                // PcdAuthState(PICC_AUTHENT1A, 1, card_KEY, ucArray_ID);
                // // 3. 写入数据到块1
                // if (PcdWrite(1, data) == MI_OK) {
                //     ESP_LOGW(TAG,"Sector 1 Write Success");
                // }


                // uint8_t buffer[16];
                // // PcdSelect(ucArray_ID);              // 选中卡
                // // 2. 认证块1
                // PcdAuthState(PICC_AUTHENT1A, 1, card_KEY, ucArray_ID);

                // // 3. 读取数据
                // if (PcdRead(1, buffer) == MI_OK) {
                //     // 打印验证（假设使用串口）
                //     ESP_LOGW(TAG,"读取结果: %c %c\n", buffer[0], buffer[1]);
                // }
                    //根据卡ID进行后续操作
                    std::string card_id = std::to_string(ucArray_ID [ 0 ]) + std::to_string(ucArray_ID [ 1 ]) + std::to_string(ucArray_ID [ 2 ]) + std::to_string(ucArray_ID [ 3 ]);
                    //输出卡ID
                    ESP_LOGI(TAG,"ID: %s", card_id.c_str());

                    if(strcmp(card_id.c_str(), CardPlayer_ID) == 0 && (device_Role != Player)) {
                        last_device_Role = device_Role;
                        device_Role = Player;
                        ESP_LOGI(TAG,"Enter Player Mode\r\n");
                        SetAecMode(kAecOff);

                    } else if(strcmp(card_id.c_str(), CardRole_Xiaozhi_ID) == 0 && (device_Role != Role_Xiaozhi)) {
                        ESP_LOGI(TAG,"Xiaozhi Role Activated\r\n");
                        last_device_Role = device_Role;
                        device_Role = Role_Xiaozhi;
                        SetAecMode(kAecOnDeviceSide);
                    } else if(strcmp(card_id.c_str(), CardRole_XiaoMing_ID) == 0 && (device_Role != Role_XiaoMing)) {
                        ESP_LOGI(TAG,"XiaoMing Role Activated\r\n");
                        last_device_Role = device_Role;
                        device_Role = Role_XiaoMing;
                        SetAecMode(kAecOnDeviceSide);
                    }
                    led->Blink(200, 200);
                    led->Blink(200, 200);
                    led->Blink(200, 200);
                    if(last_device_Role != device_Role)
                    {
                        Settings settings("device", true);
                        settings.SetInt("device_role", device_Role);
                        ESP_LOGW(TAG,"保存当前设备角色: %d", device_Role);
                        ESP_LOGW(TAG,"=================即将重启=================");
                        vTaskDelay( pdMS_TO_TICKS(1000) );
                        Reboot();
                    }

                }
                // LastUID[0] = ucArray_ID[0];
                // LastUID[1] = ucArray_ID[1];
                // LastUID[2] = ucArray_ID[2];
                // LastUID[3] = ucArray_ID[3];
            }
            #else
            #endif
            vTaskDelay( pdMS_TO_TICKS(500) );
    }
} 
// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    auto& wifi_station = WifiStation::GetInstance();
    auto music = Board::GetInstance().GetMusic();
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            Wifi_Offline = true;
            esp_timer_start_periodic(clock_Offlinetimer_handle_, 5000000);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {

            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            
            if(wifi_station.IsConnected() && (clock_ticks_ % 10 == 0))
            {
                auto Rssi = wifi_station.GetRssi();
                ESP_LOGI(TAG,"Rssi:%d dBm",Rssi);
                if(Rssi < -60)
                {
                    ESP_LOGI(TAG,"Weak Wifi Signal, Start Scanning");
                    esp_wifi_scan_start(NULL, false);
                }
            }
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                SystemInfo::PrintHeapStats();
            }   
            if(Offline_ticks_>=10)         
            {
                Offline_ticks_=0;
                esp_timer_stop(clock_Offlinetimer_handle_);
                // SetDeviceState(kDeviceStateWifiConfiguring);
            }

            // 空闲超时自动进入深度睡眠（仅在真正可以进入睡眠时）
            if (device_state_ == kDeviceStateIdle && (music->ReturnMode() == false)) {
                ESP_LOGD(TAG, "空闲计时: %d 秒", sleep_ticks_);
                sleep_ticks_++;
                if (CanEnterSleepMode() && sleep_ticks_ >= IDLE_DEEP_SLEEP_SECONDS) {
                    ESP_LOGI(TAG, "Device idle for %d seconds and can sleep -> entering deep sleep", IDLE_DEEP_SLEEP_SECONDS);
                    // 防止重复调度：清零计时
                    sleep_ticks_ = 0;
                    // 在主线程上下文调度进入深度睡眠
                    Schedule([this]() {
                        this->EnterDeepSleep();
                        ESP_LOGI(TAG, "停止主事件循环任务");
                        vTaskDelete(NULL);
                    });
                }
            }
            else if (device_state_ == kDeviceStateIdle && (music->ReturnMode() == true)) {
                if(g_duration_flag.load())
                {
                    ESP_LOGD(TAG, "有时间限制的播放模式下，不进入深度睡眠");
                    //有时间限制的播放模式下，不进入深度睡眠
                    sleep_music_ticks_ = 0;
                    continue;
                }
                ESP_LOGD(TAG, "播放空闲计时: %d 秒", sleep_music_ticks_);
                sleep_music_ticks_++;
                if (CanEnterSleepMode() && sleep_music_ticks_ >= (4*IDLE_DEEP_SLEEP_SECONDS)) {
                    ESP_LOGI(TAG, "Music idle for %d seconds and can sleep -> entering deep sleep", (4*IDLE_DEEP_SLEEP_SECONDS));
                    music->SetStopSignal(true);
                    // 防止重复调度：清零计时
                    sleep_music_ticks_ = 0;
                    // 在主线程上下文调度进入深度睡眠
                    Schedule([this]() {
                        this->EnterDeepSleep();
                        ESP_LOGI(TAG, "停止主事件循环任务");
                        vTaskDelete(NULL);
                    });
                }
            }
            else {
                sleep_music_ticks_ = 0;
                sleep_ticks_ = 0;
            }

            if(music->ReturnMode() == true)
            {
                // if(device_state_ != kDeviceStateIdle && (device_state_last_ == kDeviceStateConnecting || wake_word_detected_))
                if(wake_word_detected_)
                {
                    //开始计时
                    if (s_wake_start_ms.load(std::memory_order_acquire) == 0) {
                        StartWakeTimerInternal();
                    }
                    wake_word_detected_ = false;
                }

            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        wake_word_detected_ = true;
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    clock_ticks_ = 0;
    device_state_last_ = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);



    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    #if !my
    auto led = board.GetLed();
    led->OnStateChanged();
    #endif
    auto& wifi_station = WifiStation::GetInstance();


    
    // auto codec = Board::GetInstance().GetAudioCodec();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
                wifi_station.Stop(); // 停止当前WiFi连接
                board.EnterWifiConfigMode();
                break;
        default:
            // Do nothing
            break;
    }
}


void Application ::SendMessage(std::string &message) {
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG," Protocol not initialized");
        return;
    }
    ESP_LOGI(TAG, "Sending message: %s", message.c_str());
    //去除一些cjson中不能被识别的字符
    message.erase(std::remove(message.begin(), message.end(), '\n'), message.end());
    message.erase(std::remove(message.begin(), message.end(), '\r'), message.end());
    message.erase(std::remove(message.begin(), message.end(), '\"'), message.end());

    //空闲状态直接向服务器发送用户消息
    if(device_state_ == kDeviceStateIdle)
    {
        ToggleChatState();
        Schedule([this, message = std::move(message)]() {
        protocol_->SendWakeWordDetected(message);
        });
    }
    else if(device_state_ == kDeviceStateSpeaking)
    {
        //正在说话状态下，先中止当前的说话，然后发送用户消息
        Schedule([this, message = std::move(message)]() {
        AbortSpeaking(kAbortReasonNone);
        protocol_->SendWakeWordDetected(message);
        });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        //正在听取状态下，直接发送用户消息
        Schedule([this, message = std::move(message)]() {
        protocol_->SendWakeWordDetected(message);
        });
    }
    
}
void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

// 新增：接收外部音频数据（如音乐播放）
void Application::AddAudioData(AudioStreamPacket&& packet) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (device_state_ == kDeviceStateIdle && codec->output_enabled()) {
    //     // packet.payload包含的是原始PCM数据（int16_t）
        if (packet.payload.size() >= 2) {
            size_t num_samples = packet.payload.size() / sizeof(int16_t);
            std::vector<int16_t> pcm_data(num_samples);
            memcpy(pcm_data.data(), packet.payload.data(), packet.payload.size());
            
            // 检查采样率是否匹配，如果不匹配则进行简单重采样
            if (packet.sample_rate != codec->output_sample_rate()) {
                // ESP_LOGI(TAG, "Resampling music audio from %d to %d Hz", 
                //         packet.sample_rate, codec->output_sample_rate());
                
                // 验证采样率参数
                if (packet.sample_rate <= 0 || codec->output_sample_rate() <= 0) {
                    ESP_LOGE(TAG, "Invalid sample rates: %d -> %d", 
                            packet.sample_rate, codec->output_sample_rate());
                    return;
                }
                
                std::vector<int16_t> resampled;
                
                if (packet.sample_rate > codec->output_sample_rate()) {
                    ESP_LOGI(TAG, "Music Player: Adjust the sampling rate from %d Hz to %d Hz", 
                        codec->output_sample_rate(), packet.sample_rate);

                    // 尝试动态切换采样率
                    if (codec->SetOutputSampleRate(packet.sample_rate)) {
                        ESP_LOGI(TAG, "Successfully switched to music playback sampling rate: %d Hz", packet.sample_rate);
                    } else {
                        ESP_LOGW(TAG, "Unable to switch sampling rate, continue using current sampling rate: %d Hz", codec->output_sample_rate());
                    }
                } else {
                    if (packet.sample_rate > codec->output_sample_rate()) {
                        // 下采样：简单丢弃部分样本
                        float downsample_ratio = static_cast<float>(packet.sample_rate) / codec->output_sample_rate();
                        size_t expected_size = static_cast<size_t>(pcm_data.size() / downsample_ratio + 0.5f);
                        std::vector<int16_t> resampled(expected_size);
                        size_t resampled_index = 0;
                        
                        for (size_t i = 0; i < pcm_data.size(); ++i) {
                            if (i % static_cast<size_t>(downsample_ratio) == 0) {
                                resampled[resampled_index++] = pcm_data[i];
                            }
                        }
                        
                        pcm_data = std::move(resampled);
                        ESP_LOGI(TAG, "Downsampled %d -> %d samples (ratio: %.2f)", 
                                pcm_data.size(), resampled.size(), downsample_ratio);
                    } else if (packet.sample_rate < codec->output_sample_rate()) {
                        // 上采样：线性插值
                        float upsample_ratio = codec->output_sample_rate() / static_cast<float>(packet.sample_rate);
                        size_t expected_size = static_cast<size_t>(pcm_data.size() * upsample_ratio + 0.5f);
                        resampled.reserve(expected_size);
                    
                        for (size_t i = 0; i < pcm_data.size(); ++i) {
                            // 添加原始样本
                            resampled.push_back(pcm_data[i]);
                        
                        
                            // 计算需要插值的样本数
                            int interpolation_count = static_cast<int>(upsample_ratio) - 1;
                            if (interpolation_count > 0 && i + 1 < pcm_data.size()) {
                                int16_t current = pcm_data[i];
                                int16_t next = pcm_data[i + 1];
                                for (int j = 1; j <= interpolation_count; ++j) {
                                    float t = static_cast<float>(j) / (interpolation_count + 1);
                                    int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                                    resampled.push_back(interpolated);
                                }
                            } else if (interpolation_count > 0) {
                                // 最后一个样本，直接重复
                                for (int j = 1; j <= interpolation_count; ++j) {
                                    resampled.push_back(pcm_data[i]);
                                }
                            }
                        }

                        ESP_LOGI(TAG, "Upsampled %d -> %d samples (ratio: %.2f)", 
                            pcm_data.size(), resampled.size(), upsample_ratio);
                            pcm_data = std::move(resampled);
                    }
                }
                
                
            }
            
            // 确保音频输出已启用
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
            
            // 发送PCM数据到音频编解码器
            codec->OutputData(pcm_data);
            
            audio_service_.UpdateOutputTimestamp();
        }
    }
}

static void PlayDurationTimerCallback(void* arg) {
    // 在主线程停止播放，保证线程安全
    Application::GetInstance().Schedule([=]() {
        auto &board = Board::GetInstance();
        auto m = board.GetMusic();
        if (m) {
            ESP_LOGW(TAG, "Play duration timer expired, stopping playback");
            m->SetStopSignal(true);
            m->StopStreaming();
            m->SetMode(false);
        }
    });

    // 清理定时器对象
    esp_timer_handle_t* h = static_cast<esp_timer_handle_t*>(arg);
    if (h && *h) {
        esp_timer_stop(*h);
        esp_timer_delete(*h);
    }
    delete h;

    // 清全局指针与过期时间
    auto instance = &Application::GetInstance();
    {
        std::lock_guard<std::mutex> lk(instance->g_play_timer_mutex);
        instance->g_play_timer_handle = nullptr;
        instance->g_play_timer_expire_us.store(0);
        // 如需清除请求时长，可在此处置零：
        instance->g_requested_play_duration_sec.store(0);
        instance->g_duration_flag.store(false);
    }

    ESP_LOGW(TAG, "Play duration timer callback finished: cleared timer state");
}

void Application::StartPlayDurationTimerIfRequested() {
    int dur = g_requested_play_duration_sec.exchange(0);
    if (dur <= 0) return;
    g_duration_flag.store(true);
    ESP_LOGW(TAG, "Starting play duration timer for %d seconds", dur);
    std::lock_guard<std::mutex> lock(g_play_timer_mutex);
    if (g_play_timer_handle) {
        esp_timer_stop(*g_play_timer_handle);
        esp_timer_delete(*g_play_timer_handle);
        delete g_play_timer_handle;
        g_play_timer_handle = nullptr;
    }

    esp_timer_handle_t* th = new esp_timer_handle_t;
    esp_timer_create_args_t args;
    memset(&args, 0, sizeof(args));
    args.callback = PlayDurationTimerCallback;
    args.arg = th;
    args.name = "play_duration_timer";

    if (esp_timer_create(&args, th) != ESP_OK) {
        delete th;
        ESP_LOGW(TAG, "Failed to create play duration timer");
        return;
    }
    g_play_timer_handle = th;
    uint64_t us = static_cast<uint64_t>(dur) * 1000000ULL;
    // 记录到期时间，供 ExtendPlayDurationSeconds 读取剩余时间
    uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    g_play_timer_expire_us.store((int64_t)(now_us + us));
    esp_timer_start_once(*th, us);
    ESP_LOGI(TAG, "Started play duration timer: %d seconds (expire at %llu us)", dur, (unsigned long long)(now_us + us));
}



// 创建并启动一次性播放定时器（内部使用，调用时已持有互斥/线程安全）
bool Application::CreateAndStartPlayTimer(uint64_t us) {
    std::lock_guard<std::mutex> lock(g_play_timer_mutex);
    // 清理旧定时器
    if (g_play_timer_handle) {
        esp_timer_stop(*g_play_timer_handle);
        esp_timer_delete(*g_play_timer_handle);
        delete g_play_timer_handle;
        g_play_timer_handle = nullptr;
    }
    g_duration_flag.store(true);
    esp_timer_handle_t* th = new esp_timer_handle_t;
    esp_timer_create_args_t args;
    memset(&args, 0, sizeof(args));
    args.callback = PlayDurationTimerCallback;
    args.arg = th;
    args.name = "play_duration_timer";
    if (esp_timer_create(&args, th) != ESP_OK) {
        delete th;
        ESP_LOGW(TAG, "Failed to create play duration timer");
        return false;
    }
    g_play_timer_handle = th;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    g_play_timer_expire_us.store((int64_t)(now_us + us));
    esp_timer_start_once(*th, us);
    ESP_LOGI(TAG, "Started/Restarted play duration timer: %f s", (unsigned long long)us/1000000.0);
    return true;
}

// 在已有请求机制外，提供按秒延长播放时长的 API（线程安全）
bool Application::ExtendPlayDurationSeconds(int extra_seconds) {
    if (extra_seconds <= 0) return false;
    uint64_t extra_us = static_cast<uint64_t>(extra_seconds) * 1000000ULL;
    g_duration_flag.store(true);
    // 在持锁下采样当前定时器过期时间与是否存在定时器，计算剩余时间
    uint64_t base_remaining_us = 0;
    {
        std::lock_guard<std::mutex> lock(g_play_timer_mutex);
        uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
        int64_t expire_us = g_play_timer_expire_us.load();
        if (g_play_timer_handle && expire_us > static_cast<int64_t>(now_us)) {
            base_remaining_us = static_cast<uint64_t>(expire_us - static_cast<int64_t>(now_us));
            ESP_LOGI(TAG, "Extending existing play timer: +%d s, remaining %llu us",
                     extra_seconds, (unsigned long long)base_remaining_us);
        } else {
            base_remaining_us = 0;
            ESP_LOGI(TAG, "No existing play timer, creating new one for %d s", extra_seconds);
        }
    } // 释放锁 — 现在安全调用会再次锁的函数

    uint64_t new_total_us = base_remaining_us + extra_us;
    return CreateAndStartPlayTimer(new_total_us);
}

void Application::StopPlayDurationTimer()
{
   std::lock_guard<std::mutex> lock(g_play_timer_mutex);
   if (g_play_timer_handle) {
       esp_timer_stop(*g_play_timer_handle);
       esp_timer_delete(*g_play_timer_handle);
       delete g_play_timer_handle;
       g_play_timer_handle = nullptr;
   }
   g_play_timer_expire_us.store(0);
   Set_PlayDuration(0);
   g_duration_flag.store(false);
}