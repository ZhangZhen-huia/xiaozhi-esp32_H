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


#define TAG "Zegbot"
extern std::atomic<bool> triple_press_window_expired;


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

class ZEGBOT : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_Boot_IO0;
    int battery_ = 0;
    bool longpress_flag_ = false;
    uint8_t click_count = 0;
    sdmmc_card_t *sdcard_ = nullptr;
    #if my
    Pca9557* pca9557_;
    #else
    
    Led led_;
    #endif
    static constexpr gpio_num_t VIBRATION_GPIO = GPIO_NUM_41;
    static constexpr int VIBRATION_WINDOW_MS = 500;      // 统计窗口 (毫秒)
    static constexpr int VIBRATION_THRESHOLD = 60;        // 触发阈值 (次)
    uint32_t last_trigger_time_ = 0;           // 上次触发的时间戳（毫秒）
    uint32_t trigger_min_interval_ms_ = 3000;     // 最小触发间隔（毫秒），默认 3000
    uint8_t sleep_mode = 0; // 0-不睡眠，1-30分钟，2-60分钟，3-120分钟
    // 振动检测相关成员
    volatile uint32_t vibration_pulse_count_ = 0;
    bool vibration_triggered_ = false;
    TimerHandle_t vibration_timer_ = nullptr;
    static portMUX_TYPE vibration_spinlock;   // 声明静态自旋锁
    // 私有方法
    static void IRAM_ATTR vibration_isr_handler(void *arg);
    static void vibration_timer_callback(TimerHandle_t xTimer);
    void init_vibration_sensor();


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


    std::atomic<int64_t> last_click_ms_{0};
    esp_timer_handle_t click_timer_ = nullptr;

    void ExecSingleClick() {
        auto& app = Application::GetInstance();
        auto music = Board::GetMusic();
        bool has_net = WifiStation::GetInstance().IsConnected();
        bool has_rfid = app.have_rfid_;
        auto role = app.GetDeviceFunction();
        bool is_ai_mode = (role == Function_AIAssistant);
        bool is_music_mode = (role == Function_MusicStory);

        //无公仔
        if(!has_rfid)
        {
            app.PlaySound(Lang::Sounds::OGG_PLACERFID); // 无公仔:放置公仔
        }

        //有公仔
        else if(has_rfid)
        {
            //有公仔+无网
            if(!has_net)
            {
                app.PlaySound(Lang::Sounds::OGG_WIFICONFIG);//配网
            }
            //有公仔+有网
            else if(has_net)
            {
                app.ToggleChatState();//切换状态
            }
        }
    }

    void ExecDoubleClick() {
        auto& app = Application::GetInstance();
        auto music = Board::GetMusic();
        bool has_net = WifiStation::GetInstance().IsConnected();
        bool has_rfid = app.have_rfid_;
        auto role = app.GetDeviceFunction();
        bool is_ai_mode = (role == Function_AIAssistant);
        bool is_music_mode = (role == Function_MusicStory);
        bool is_light_mode = (role == Function_Light);
        //无公仔
        if(!has_rfid)
        {
            if(!is_light_mode)
                app.PlaySound(Lang::Sounds::OGG_PLACERFID); // 无公仔:放置公仔
            else
            {
                sleep_mode++;
                if(sleep_mode > 3) sleep_mode = 0;
                
                if (sleep_mode == 0) {
                    ESP_LOGI(TAG, "Boot按键 双击：30分钟");
                    app.Night_light_sleep_time = 30 * 60;
                    app.PlaySound(Lang::Sounds::OGG_30MIN);
                } else if (sleep_mode == 1) {
                    ESP_LOGI(TAG, "Boot按键 双击：60分钟");
                    app.Night_light_sleep_time = 60 * 60;
                    app.PlaySound(Lang::Sounds::OGG_ONEHOUR);
                } else if (sleep_mode == 2) {
                    ESP_LOGI(TAG, "Boot按键 双击：120分钟");
                    app.Night_light_sleep_time = 120 * 60;
                    app.PlaySound(Lang::Sounds::OGG_TWOHOUR);
                } else {
                    ESP_LOGI(TAG, "Boot按键 双击：不睡眠");
                    app.Night_light_sleep_time = 0xFFFFFFFF;
                    app.PlaySound(Lang::Sounds::OGG_NEVERPOWEROFF); 
                }

                if(sleep_mode < 3) {
                    uint64_t us = static_cast<uint64_t>(app.Night_light_sleep_time) * 1000000ULL;
                    app.CreateAndStartPlayTimer(us);
                } else {
                    app.StopPlayDurationTimer();
                }
            }
        }
        //有公仔
        else
        {
            if(!has_net) {
                if(!is_light_mode)
                    app.PlaySound(Lang::Sounds::OGG_WIFICONFIG); // 无网+有公仔:配网
                else 
                {
                    sleep_mode++;
                    if(sleep_mode > 3) sleep_mode = 0;
                    
                    if (sleep_mode == 0) {
                        ESP_LOGI(TAG, "Boot按键 双击：30分钟");
                        app.Night_light_sleep_time = 30 * 60;
                        app.PlaySound(Lang::Sounds::OGG_30MIN);
                    } else if (sleep_mode == 1) {
                        ESP_LOGI(TAG, "Boot按键 双击：60分钟");
                        app.Night_light_sleep_time = 60 * 60;
                        app.PlaySound(Lang::Sounds::OGG_ONEHOUR);
                    } else if (sleep_mode == 2) {
                        ESP_LOGI(TAG, "Boot按键 双击：120分钟");
                        app.Night_light_sleep_time = 120 * 60;
                        app.PlaySound(Lang::Sounds::OGG_TWOHOUR);
                    } else {
                        ESP_LOGI(TAG, "Boot按键 双击：不睡眠");
                        app.Night_light_sleep_time = 0xFFFFFFFF;
                        app.PlaySound(Lang::Sounds::OGG_NEVERPOWEROFF); 
                    }

                    if(sleep_mode < 3) {
                        uint64_t us = static_cast<uint64_t>(app.Night_light_sleep_time) * 1000000ULL;
                        app.CreateAndStartPlayTimer(us);
                    } else {
                        app.StopPlayDurationTimer();
                    }

                }
            } else {
                // if(is_music_mode || is_light_mode)
                    if (music && app.All_Finish) 
                        music->SetEventNextPlay();//切换音乐
  
            }
            
        }

    }

    void InitializeButtons() {
        auto& app = Application::GetInstance();

        auto click_timer_cb = [](void* arg) {
            auto board = static_cast<ZEGBOT*>(arg);
            auto& app = Application::GetInstance();
            app.Schedule([board]() {
                if (board->last_click_ms_.load(std::memory_order_relaxed) != 0) {
                    board->last_click_ms_.store(0, std::memory_order_relaxed);
                    ESP_LOGI(TAG, "定时器检测：单次点击确认（双击超时）");
                    board->ExecSingleClick();
                }
            });
        };

        if (click_timer_ == nullptr) {
            esp_timer_create_args_t args = {};
            args.callback = click_timer_cb;
            args.arg = this;
            args.name = "boot_click_tmr";
            args.dispatch_method = ESP_TIMER_TASK;
            esp_timer_create(&args, &click_timer_);
        }

        const int64_t double_click_threshold_ms = 1000; // 调整为 1000ms 以获得更好的双击确认手感

        // 将单击与双击整合到一个硬件点击事件，由软件定时器判定阈值内合成
        boot_button_Boot_IO0.OnClick([this, &app, double_click_threshold_ms]() {
            if(!triple_press_window_expired.load(std::memory_order_acquire)) {
                click_count++;
                ESP_LOGW(TAG, "Boot按键 物理单击识别，计数：%d", click_count);
                if(click_count >= 3) {
                    click_count = 0;
                        ResetWifiConfiguration();
                    }
            }
            app.Resetsleep_music_ticks_();
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t prev_ms = last_click_ms_.load(std::memory_order_relaxed);

            // 如果距离上一次点击在阈值内，算作合成双击
            if (prev_ms != 0 && (now_ms - prev_ms) < double_click_threshold_ms) {
                // 停止定时器并重置
                if (click_timer_) esp_timer_stop(click_timer_);
                last_click_ms_.store(0, std::memory_order_relaxed);
                
                ESP_LOGI(TAG, "定时器检测：触发软件合成 双击");
                ExecDoubleClick();
            } else {
                // 第一下点击，先不触发动作，只记录并开始定时
                last_click_ms_.store(now_ms, std::memory_order_relaxed);
                if (click_timer_) {
                    esp_timer_start_once(click_timer_, double_click_threshold_ms * 1000);
                }
            }
        });

        // 硬件底层判断的双击，防丢支持
        boot_button_Boot_IO0.OnDoubleClick([this, &app]() {

            if(!triple_press_window_expired.load(std::memory_order_acquire)) {
                click_count += 2;
                ESP_LOGW(TAG, "Boot按键 物理双击识别，计数：%d", click_count);
                if(click_count >= 3) {
                    click_count = 0;

                        ResetWifiConfiguration();
                    }
                }
                
            

            app.Resetsleep_music_ticks_();
            // 防止软件合成也重复执行
            last_click_ms_.store(0, std::memory_order_relaxed);
            if (click_timer_) esp_timer_stop(click_timer_);
            
            ESP_LOGI(TAG, "硬件底层检测：触发 双击");
            ExecDoubleClick();
        });

        // 硬件底层判断的多击 (如三连击)
        boot_button_Boot_IO0.OnMultipleClick([this, &app]() {
            ESP_LOGW(TAG, "Boot按键 硬件底层检测：触发 三连击");
            app.Resetsleep_music_ticks_();
            if(!triple_press_window_expired.load(std::memory_order_acquire)) {

                // 防止软件合成也重复执行
                last_click_ms_.store(0, std::memory_order_relaxed);
                if (click_timer_) esp_timer_stop(click_timer_);
                ResetWifiConfiguration();
            }
        }, 3);

        // 长按处理
        boot_button_Boot_IO0.OnLongPressStart([this, &app]() {
            bool has_net = WifiStation::GetInstance().IsConnected();
            bool has_rfid = app.have_rfid_;
            bool is_ai_mode = (app.GetDeviceFunction() == Function_AIAssistant);
            bool is_light_mode = (app.GetDeviceFunction() == Function_Light);
            if (!has_rfid) {
                if(!is_light_mode)
                    app.PlaySound(Lang::Sounds::OGG_PLACERFID);
            }
            else if (has_rfid) {
                if(!has_net) {
                        app.PlaySound(Lang::Sounds::OGG_WIFICONFIG);
                }
                else
                {
                    auto device_state = app.GetDeviceState();
                    if(device_state != kDeviceStateIdle) {
                        app.SetDeviceState(kDeviceStateIdle);
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }

                    // 强制清空软件双击窗口缓存，防止串联判定
                    last_click_ms_.store(0, std::memory_order_relaxed);
                    if (click_timer_) esp_timer_stop(click_timer_);
                    
                    app.StartListening();
                    longpress_flag_ = true;
                    app.Resetsleep_music_ticks_();
                    ESP_LOGI(TAG, "Boot长按触发: 开始聆听");
                }
            } 
        });

        boot_button_Boot_IO0.OnPressUp([this, &app]() {
            if (longpress_flag_) {
                app.StopListening();
                longpress_flag_ = false;
                ESP_LOGI(TAG, "Boot释放: 停止聆听");
            }
            app.Resetsleep_music_ticks_();
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
        sdcard_ = card; // 保存已挂载的 sd 卡指针
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
                auto board = static_cast<ZEGBOT*>(user_data);
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
                                    #if battery_check
                                    ESP_LOGI(TAG, "电量过低，强制停止播放音乐");
                                    music->SetMode(false);
                                    if(music->IsPlaying())
                                        music->StopStreaming(); // 停止当前播放
                                    vTaskDelay(pdMS_TO_TICKS(1000));
                                    app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                    app.PlaySound(Lang::Sounds::OGG_LOWBATTERY);
                                    #endif
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
                                                    app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                                                }
                                            else 
                                                ESP_LOGI(TAG, "音乐未暂停，跳过低电量提示音");   
                                            vTaskDelay(pdMS_TO_TICKS(3000));
                                            music->ResumePlayback();
                                        }
                                        else 
                                        {
                                            app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                                        }
                                    }
                                    else 
                                    {   
                                        app.AbortSpeaking(AbortReason::kAbortReasonNone);
                                        app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
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

    ZEGBOT():
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
        RC522_Config_Type('A');
        InitializeSwitches();
        
        // 新增：初始化振动传感器
        init_vibration_sensor();
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
    virtual void Deinitialize() override {
        // 在此处添加任何必要的反初始化代码
        ESP_LOGI(TAG, "Zegbot 反初始化");
        if (i2c_bus_) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
        }
        

        // 3.1 先输出低电平
        gpio_set_direction(AUDIO_CODEC_I2C_SDA_PIN, GPIO_MODE_OUTPUT);
        gpio_set_direction(AUDIO_CODEC_I2C_SCL_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(AUDIO_CODEC_I2C_SDA_PIN, 0);
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 0);
        delay_ms(1);
        
        // 3.2 然后输出高电平
        gpio_set_level(AUDIO_CODEC_I2C_SDA_PIN, 1);
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
        delay_ms(1);
        
        // 3.3 最后设为高阻态
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << AUDIO_CODEC_I2C_SDA_PIN) | (1ULL << AUDIO_CODEC_I2C_SCL_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        
        // 步骤4：读取并记录状态
        delay_ms(5);
        int sda = gpio_get_level(AUDIO_CODEC_I2C_SDA_PIN);
        int scl = gpio_get_level(AUDIO_CODEC_I2C_SCL_PIN);

        ESP_LOGI("I2C", "总线释放后: SDA=%d, SCL=%d", sda, scl);
        
        if (sda == 1 && scl == 1) {
            ESP_LOGI("I2C", "✅ 总线已成功释放");
        } else {
            ESP_LOGW("I2C", "⚠️ 总线可能仍被占用");
            if (sda == 0) ESP_LOGW("I2C", "   SDA被拉低，可能ES8311/ES7210未关闭");
            if (scl == 0) ESP_LOGW("I2C", "   SCL被拉低，检查其他I2C设备");
        }
        // 取消挂载 SD 卡（若已挂载）
        if (sdcard_) {
            ESP_LOGI(TAG, "Unmounting SD card at %s", MOUNT_POINT);
            esp_err_t rc = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, sdcard_);    
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(rc));
            }
            sdcard_ = nullptr;
            ESP_LOGI(TAG, "SD card unmounted");
        } else {
            ESP_LOGI(TAG, "No SD card mounted");
        }
        // 6. 删除SDMMC主机驱动（关键！）
        ESP_LOGI(TAG, "删除SDMMC主机驱动");
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.deinit_p(host.slot);  // 释放SDMMC外设


        gpio_deep_sleep_hold_dis();  // 全局禁用深度睡眠保持
        // CLK引脚：输出低电平（防止时钟线浮动）
        if (BSP_SD_CLK != GPIO_NUM_NC) {
            gpio_reset_pin(BSP_SD_CLK);
            gpio_set_direction(BSP_SD_CLK, GPIO_MODE_OUTPUT);
            gpio_set_level(BSP_SD_CLK, 0);
            gpio_set_pull_mode(BSP_SD_CLK, GPIO_FLOATING);
            gpio_hold_dis(BSP_SD_CLK);
            ESP_LOGD(TAG, "CLK引脚配置为输出低电平");
        }
        
        // CMD引脚：输入高阻态（外部上拉）
        if (BSP_SD_CMD != GPIO_NUM_NC) {
            gpio_reset_pin(BSP_SD_CMD);
            gpio_set_direction(BSP_SD_CMD, GPIO_MODE_INPUT);
            gpio_set_pull_mode(BSP_SD_CMD, GPIO_FLOATING);
            gpio_hold_dis(BSP_SD_CMD);
            ESP_LOGD(TAG, "CMD引脚配置为输入高阻态");
        }
        if (BSP_SD_D0 != GPIO_NUM_NC) {
            gpio_reset_pin(BSP_SD_D0);
            gpio_set_direction(BSP_SD_D0, GPIO_MODE_INPUT);
            gpio_set_pull_mode(BSP_SD_D0, GPIO_FLOATING);
            gpio_hold_dis(BSP_SD_D0);
            ESP_LOGD(TAG, "DATA引脚配置为输入高阻态");
        }
    }
};

portMUX_TYPE ZEGBOT::vibration_spinlock = portMUX_INITIALIZER_UNLOCKED;
void ZEGBOT::init_vibration_sensor()
{
    // 配置 GPIO37 为输入，上拉
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VIBRATION_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,   // 上升沿+下降沿都触发
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // 安装 GPIO 中断服务（如果尚未安装，检查是否已有其他中断）
    static bool isr_installed = false;
    if (!isr_installed) {
        ESP_ERROR_CHECK(gpio_install_isr_service(0));
        isr_installed = true;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(VIBRATION_GPIO, vibration_isr_handler, this));

    // 创建软件定时器，周期 = VIBRATION_WINDOW_MS，自动重载
    vibration_timer_ = xTimerCreate("vib_timer",
                                    pdMS_TO_TICKS(VIBRATION_WINDOW_MS),
                                    pdTRUE,
                                    this,
                                    vibration_timer_callback);
    if (vibration_timer_ == NULL) {
        ESP_LOGE(TAG, "创建振动定时器失败");
        return;
    }
    xTimerStart(vibration_timer_, portMAX_DELAY);

    ESP_LOGI(TAG, "振动传感器已初始化: GPIO%d, 窗口=%dms, 阈值=%d次",
             VIBRATION_GPIO, VIBRATION_WINDOW_MS, VIBRATION_THRESHOLD);
}

// 中断服务函数（IRAM 安全）
void IRAM_ATTR ZEGBOT::vibration_isr_handler(void *arg)
{
    ZEGBOT *self = static_cast<ZEGBOT*>(arg);
    portENTER_CRITICAL_ISR(&vibration_spinlock);
    self->vibration_pulse_count_++;
    portEXIT_CRITICAL_ISR(&vibration_spinlock);
}

void ZEGBOT::vibration_timer_callback(TimerHandle_t xTimer)
{
    auto &board = Board::GetInstance();
    auto music = board.GetMusic();


    ZEGBOT *self = static_cast<ZEGBOT*>(pvTimerGetTimerID(xTimer));
    if (self == nullptr) return;

    uint32_t count;
    portENTER_CRITICAL(&vibration_spinlock);
    count = self->vibration_pulse_count_;
    self->vibration_pulse_count_ = 0;
    portEXIT_CRITICAL(&vibration_spinlock);

    // 获取当前系统时间（毫秒）
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // 检查是否满足触发条件
    if (count >= self->VIBRATION_THRESHOLD) {
        // 判断时间间隔
        if (now_ms - self->last_trigger_time_ >= self->trigger_min_interval_ms_) {
            // 满足间隔，执行触发动作
            self->last_trigger_time_ = now_ms;
            ESP_LOGI(TAG, "*** 振动触发！跳变次数 %lu >= %d ***", count, self->VIBRATION_THRESHOLD);
            auto led = board.GetLed();
            // // 执行触发动作（例如播放提示音）
            auto &app = Application::GetInstance();
            {
                ESP_LOGW(TAG, "拍一拍触发：切换夜灯亮度");
                static const uint8_t levels[] = {0,25,50,100};
                static int idx = 0;
                static int dir = 1; // 1: 向上，-1: 向下
                uint8_t cur = board.GetBacklight()->brightness();
                int found = -1;
                for (int i = 0; i < 4; ++i) {
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
                    if (idx < 3) {
                        idx++;
                    } else {
                        // 到顶后切换到下降方向，下一档为 50
                        idx = 2;
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
                ESP_LOGI(TAG, "Nightlight brightness: %u -> %u", (unsigned)cur, (unsigned)levels[idx]);
                board.GetBacklight()->SetBrightness(levels[idx], true, true);

            }

            // 播放提示音或执行其他任务
            ESP_LOGE(TAG, "振动触发动作已执行");
  
        } else {
            // 还在冷却期内，忽略本次触发，仅记录日志
            ESP_LOGD(TAG, "振动触发被忽略（冷却中，剩余 %lu ms）",
                     self->trigger_min_interval_ms_ - (now_ms - self->last_trigger_time_));
        }
    } else if (count > 0) {
        ESP_LOGD(TAG, "窗口内检测到 %lu 次跳变（未达阈值）", count);
    }
}

DECLARE_BOARD(ZEGBOT);