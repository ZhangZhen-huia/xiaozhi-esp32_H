#include "wifi_board.h"
#include "codecs/box_audio_codec.h"

#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "driver/gpio.h"
#include <dirent.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <lvgl.h>
#include <cmath>
#include "esp32_music.h"
#include "esp32_rc522.h"
#include "bat_monitor.h"
#include "led.h"

#include "assets/lang_config.h"

extern uint8_t data[16];

#define TAG "LichuangDevBoard"

#if my
class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};
#else

#endif

class CustomAudioCodec : public BoxAudioCodec {
    #if my
private:
    Pca9557* pca9557_;
    #endif

public:
    #if my
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_NC, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE),
                        pca9557_(pca9557){
    }

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
    #else
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_11, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE)
    {
    }

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            gpio_set_level(GPIO_NUM_11, 1);
        } else {
            gpio_set_level(GPIO_NUM_11, 0);
        }
    }
    #endif
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_Boot_IO0;
    int battery_ = 0;
    bool longpress_flag_ = false;
    #if my
    Pca9557* pca9557_;
    #else
    
    Led led_;
    #endif

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                #if my
                .enable_internal_pullup = 1,
                #else
                .enable_internal_pullup = 0,
                #endif
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        #if my
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
        #endif
    }

    void InitializeButtons() {
        
        // 处理单击/双击/长按逻辑：第一次单击立即切换状态；若在阈值内再次单击，则恢复到原始状态并执行双击动作
        static esp_timer_handle_t click_timer = nullptr;
        static std::atomic<int64_t> last_click_ms{0};
        static std::atomic<int> pending_prev_state{-1};
        const int64_t double_click_threshold_ms = 1500; // 双击阈值，可调整

        // 定时器回调：超时后清除 pending 状态（表示双击窗口已过）
        auto click_timer_cb = [](void* arg) {
            // 清除等待标记，表示不会再还原
            last_click_ms.store(0, std::memory_order_relaxed);
            pending_prev_state.store(-1, std::memory_order_relaxed);
            ESP_LOGD(TAG, "Boot按键 单击确认超时，清除待恢复状态");
        };

        if (click_timer == nullptr) {
            esp_timer_create_args_t args;
            args.callback = click_timer_cb;
            args.arg = this;
            args.name = "boot_click_tmr";
            args.dispatch_method = ESP_TIMER_TASK;
            args.skip_unhandled_events = false;
            esp_timer_create(&args, &click_timer);
        }

        // 单击处理：
        boot_button_Boot_IO0.OnClick([this]() {
            data[0] = 91;  // 清零缓冲区
            data[1] = 91;  // 清零缓冲区
            auto& app = Application::GetInstance();
            // 获取设备功能
            auto device_function = app.GetDeviceFunction();
            
            if (device_function == Function_Light) {
                ESP_LOGW(TAG, "Boot按键 单击：切换夜灯亮度");



                static const uint8_t levels[] = {0, 25, 50, 75, 100};
                static int idx = 0;
                static int dir = 1; // 1: 向上，-1: 向下
                uint8_t cur = GetBacklight()->brightness();
                int found = -1;
                for (int i = 0; i < 5; ++i) {
                    if (cur == levels[i]) { found = i; break; }
                }
                if (found != -1) {
                    idx = found;
                } else {
                    // 如果当前不在四档之一，从最低档开始
                    idx = 0;
                    dir = 1;
                }
                if (dir == 1) {
                    if (idx < 4) {
                        idx++;
                    } else {
                        // 到顶后切换到下降方向，下一档为 75
                        idx = 3;
                        dir = -1;
                    }
                } else {
                    if (idx > 0) {
                        idx--;
                    } else {
                        // 到底后切换回上升方向，下一档为 25
                        idx = 1;
                        dir = 1;
                    }
                }
                    data[0] = 91+idx;  //
                    data[1] = 91+idx;  //
                    ESP_LOGW(TAG, "%d %d",data[0],data[1]);
                ESP_LOGI(TAG, "Nightlight brightness: %u -> %u", (unsigned)cur, (unsigned)levels[idx]);
                GetBacklight()->SetBrightness(levels[idx], true, true);

            } else if (device_function == Function_AIAssistant) {

                app.Resetsleep_music_ticks_();
                int64_t now_ms = esp_timer_get_time() / 1000;
                int64_t prev_ms = last_click_ms.load(std::memory_order_relaxed);



                // 如果上一次单击存在且在阈值内 -> 视为双击：恢复原始状态并执行双击动作
                if (prev_ms != 0 && (now_ms - prev_ms) < double_click_threshold_ms) {
                    // 停掉定时器
                    if (click_timer) esp_timer_stop(click_timer);

                    int prev_state = pending_prev_state.load(std::memory_order_relaxed);
                    // 若记录了原始状态，则再次切换（Toggle）回去
                    if (prev_state != -1) {
                        // 调度回主线程执行恢复，避免在中断/其他上下文直接调用
                        app.Schedule([&app]() {
                            ESP_LOGI(TAG, "Boot按键 双击：恢复到上一次状态（由双击触发）");
                            app.ToggleChatState(); // 第二次 Toggle 恢复到原始状态
                        });
                    } else {
                        ESP_LOGW(TAG, "Boot按键 双击检测到但没有记录原始状态，跳过恢复");
                    }

                    // 执行原有的双击行为（如音乐切换）
                    auto music = Board::GetMusic();
                    if (music && music->ReturnMode()) {
                        music->SetEventNextPlay();
                        ESP_LOGI(TAG, "Boot按键 双击触发: 下一首/下一个章节");
                    } else {
                        ESP_LOGI(TAG, "Boot按键 双击触发: 非音乐模式，无其他操作");
                    }

                    // 清理标记
                    last_click_ms.store(0, std::memory_order_relaxed);
                    pending_prev_state.store(-1, std::memory_order_relaxed);
                    return;
                }

                // 否则这是第一次单击：记录当前状态，立即切换，并启动定时器等待可能的第二次单击
                int cur_state = app.GetDeviceState();
                pending_prev_state.store(cur_state, std::memory_order_relaxed);

                // 立即切换状态（第一次单击即时生效）
                app.Schedule([&app]() {
                    ESP_LOGI(TAG, "Boot按键 单击：立即切换聊天状态，当前=%d", app.GetDeviceState());
                    app.ToggleChatState();
                });

                // 记录时间并启动定时器：在阈值期内若未发生第二次单击则不再恢复
                last_click_ms.store(now_ms, std::memory_order_relaxed);
                if (click_timer) {
                    esp_timer_start_once(click_timer, double_click_threshold_ms * 1000); // 单位微秒
                }
            }
        });

        // 长按开始：进入监听，清除 pending 单击以避免冲突
        boot_button_Boot_IO0.OnLongPressStart([this]() {
            // 清除待恢复标记，避免与长按冲突
            last_click_ms.store(0, std::memory_order_relaxed);
            pending_prev_state.store(-1, std::memory_order_relaxed);
            auto& app = Application::GetInstance();
            auto device_state = app.GetDeviceState();
            if(device_state != kDeviceStateIdle) {
                // 已在监听则先停止
                app.SetDeviceState(kDeviceStateIdle);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            app.StartListening();
            longpress_flag_ = true;
            ESP_LOGI(TAG, "Boot按键长按开始");
            app.Resetsleep_music_ticks_();
        });

        // 长按释放：停止监听
        boot_button_Boot_IO0.OnPressUp([this]() {
            auto& app = Application::GetInstance();
            if (longpress_flag_) {
                app.StopListening();
                longpress_flag_ = false;
                ESP_LOGI(TAG, "Boot按键长按释放：停止监听");
            }
            app.Resetsleep_music_ticks_();
        });

        // 保留 OnDoubleClick 回调（如果仍需），但避免与上面双击路径重复触发
        boot_button_Boot_IO0.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            app.Resetsleep_music_ticks_();
            // int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t handled_ms = last_click_ms.load(std::memory_order_relaxed);
            // 如果双击已由上面的单击路径处理（在同一时间窗口内），则忽略此回调
            if (handled_ms == 0) {
                // 如果这里触发，说明硬件/库直接检测到双击（非我们用两次单击合成）
                auto music = Board::GetMusic();
                if (music && music->ReturnMode()) {
                    music->SetEventNextPlay();
                    ESP_LOGI(TAG, "Boot按键 双击回调触发: 下一首/下一个章节");
                }
            } else {
                ESP_LOGI(TAG, "Boot按键 双击回调被忽略（已由单击路径处理或等待中）");
            }
        });
    }

    void InitializeSdcard() {
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 10,
            .allocation_unit_size = 20 * 1024
        };
        
        sdmmc_card_t *card;
        const char mount_point[] = MOUNT_POINT;
        ESP_LOGD(TAG, "Initializing SD card");
        ESP_LOGD(TAG, "Using SDMMC peripheral");
    
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;
        slot_config.clk = BSP_SD_CLK; 
        slot_config.cmd = BSP_SD_CMD;
        slot_config.d0 = BSP_SD_D0;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to mount filesystem. ");
            } else {
                ESP_LOGE(TAG, "Failed to initialize the card (%s). ", esp_err_to_name(ret));
            }
            return;
        }
        ESP_LOGI(TAG, "Filesystem mounted");
        sdmmc_card_print_info(stdout, card);
    }

    void InitializeSwitches()
    {
        gpio_config_t io_conf;
        // 配置为输入模式，带上拉电阻
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << LEDMODE_GPIO) | (1ULL << NORMALMODE_GPIO);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }
    void InitializeLed()
    {
        // 仅确保 LED 初始为灭（实际初始化在构造函数初始化列表完成）
        #if !my
        led_.Set(false);
        #endif
    }

    void InitializeBatteryMonitor() {
        bat_monitor_config_t config = {
            .adc_ch = ADC_CHANNEL_6,
            .charge_io = GPIO_NUM_NC,
            .v_div_ratio = 2.0f,
            .v_min = 3.67f,
            .v_max = 4.0f,
            .low_thresh = 20.0f,
            .report_ms = 5000
        };
        battery_handle = bat_monitor_create(&config);
        if (!battery_handle) {
            ESP_LOGE(TAG, "电池监测初始化失败");
            return;
        }

        bat_monitor_set_event_cb(battery_handle,
            [](bat_monitor_event_t event, float voltage, int percentage, void *user_data) {
                auto board = static_cast<LichuangDevBoard*>(user_data);
                auto music = board->GetMusic();
                auto &app = Application::GetInstance();
                static uint8_t tick = 0;
                switch (event) {
                    case BAT_EVENT_VOLTAGE_REPORT:
                        ESP_LOGI(TAG, "电池电量: %.2fV  %d%%", voltage, percentage);
                        board->battery_ = percentage;
                        break;
                    case BAT_EVENT_FULL:
                        ESP_LOGI(TAG, "电池已充满: %.2fV  %d%%", voltage, percentage);
                        break;
                    case BAT_EVENT_LOW:
                        tick++;
                        ESP_LOGI(TAG, "电池电量低: %.2fV  %d%%", voltage, percentage);
                        // 每2分钟提醒一次
                        if(tick%24==0) { 
                            tick =0;
                            if(music)
                            {
                                if(percentage <= 10)
                                {
                                    ESP_LOGI(TAG, "电量过低，强制停止播放音乐");
                                    music->SetMode(false);
                                    if(music->IsPlaying())
                                        music->StopStreaming(); // 停止当前播放
                                    vTaskDelay(pdMS_TO_TICKS(1000));
                                    app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                    app.PlaySound(Lang::Sounds::OGG_LOWBATTERY);
                                }
                                else 
                                {
                                    if(music->ReturnMode()) {
                                        if(music->IsPlaying())
                                        {
                                        music->PausePlayback();
                                            vTaskDelay(pdMS_TO_TICKS(1000));
                                            if(music->IsActualPaused()) 
                                                {
                                                    app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                                    app.PlaySound(Lang::Sounds::OGG_LOWBATTERY);
                                                }
                                            else 
                                                ESP_LOGI(TAG, "音乐未暂停，跳过低电量提示音");   
                                            vTaskDelay(pdMS_TO_TICKS(3000));
                                            music->ResumePlayback();
                                        }
                                        else 
                                        {
                                            app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                            app.PlaySound(Lang::Sounds::OGG_LOWBATTERY);
                                        }
                                    }
                                    else 
                                    {   
                                        app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                        app.PlaySound(Lang::Sounds::OGG_LOWBATTERY);
                                    }
                                }
                            }
                        }
                        break;
                    case BAT_EVENT_CHARGING_BEGIN:
                        ESP_LOGI(TAG, "开始充电");
                        break;
                    case BAT_EVENT_CHARGING_STOP:
                        ESP_LOGI(TAG, "停止充电");
                        break;
                }
            }, this);
        ESP_LOGI(TAG, "电池监测已启动");
    };

public:

    LichuangDevBoard():
    #if !my
        boot_button_Boot_IO0(BOOT_BUTTON_GPIO),
        led_(GPIO_NUM_6) // 直接在成员初始化中绑定 IO6
    #else
        boot_button_Boot_IO0(BOOT_BUTTON_GPIO)
    #endif
    {
        InitializeI2c();
        InitializeSdcard();
        InitializeButtons();
        InitializeLed();
        #if !my
        InitializeBatteryMonitor();
        #endif
        RC522_Init();
        RC522_Rese(); // 复位RC522
        InitializeSwitches();
    }
#if my
    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }
#else
    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_);
        return &audio_codec;
    }
    virtual Led* GetLed() override {
        return &led_;
    }
#endif
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
    virtual int GetBatteryLevel() override {
        return battery_;
    }
};

DECLARE_BOARD(LichuangDevBoard);