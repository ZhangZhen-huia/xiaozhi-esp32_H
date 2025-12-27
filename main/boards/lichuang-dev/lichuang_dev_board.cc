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
    // Pca9557* pca9557_ = nullptr;
        Pca9557* pca9557_;
        #else

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
   
        // if (enable) {
        //     gpio_set_level(GPIO_NUM_11, 1);  // 设置高电平
        //     // pca9557_->SetOutputState(1, 1);
        // } else {
        //      gpio_set_level(GPIO_NUM_11, 0);  // 设置低电平
        //     // pca9557_->SetOutputState(1, 0);
        // }
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
            gpio_set_level(GPIO_NUM_11, 1);  // 设置高电平
            // pca9557_->SetOutputState(1, 1);
        } else {
             gpio_set_level(GPIO_NUM_11, 0);  // 设置低电平
            // pca9557_->SetOutputState(1, 0);
        }
    }
    #endif
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_Boot_IO0;
    Button boot_button_IO6;

    #if my
 Pca9557* pca9557_;
 #else
 
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
#else

#endif
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {

        boot_button_IO6.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }

            // 限流：防止连续快速触发导致并发问题（可调整间隔 ms）
            static std::atomic<int64_t> last_trigger_ms{0};
            const int64_t min_interval_ms = 3000; // 3000ms 限流
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t prev = last_trigger_ms.load(std::memory_order_relaxed);

            bool should_send = false;
            if (now_ms - prev >= min_interval_ms) {
                // 达到间隔，允许触发并更新时间戳
                last_trigger_ms.store(now_ms, std::memory_order_relaxed);
                should_send = true;
            } else {
                ESP_LOGW(TAG, "NextPlay ignored due to rapid press");
                // 不 return，继续执行该回调的其余逻辑（如果有）
            }

            // 仅在允许时发送“下一首”消息，回调本身不会提前返回
            if (should_send) {
                auto music = Board::GetMusic();
                if (music && music->ReturnMode()) {

                    music->SetMusicEventNextPlay();
                }
                else {
                    app.ToggleChatState();
                }
            }

            // 回调其余逻辑（如需）继续放在这里，不会因为限流而被跳过
        });
                

#if !my
        boot_button_Boot_IO0.OnClick([this](){
            auto& app = Application::GetInstance();
            app.ToggleChatState();            
        });
#endif
        // //Boot按键
        // boot_button_Boot_IO0.OnLongPressStart([this](){
        //     auto& app = Application::GetInstance();
        //     app.StartListening();
        // });
        // boot_button_Boot_IO0.OnPressUp([this](){
        //     auto& app = Application::GetInstance();
        //     app.StopListening();
        // });
#if CONFIG_USE_DEVICE_AEC
        boot_button_Boot_IO0.OnDoubleClick([this]() {

                auto music = Board::GetMusic();
                if (music && music->ReturnMode()) {

                    music->SetMusicEventNextPlay();
                }
            // auto& app = Application::GetInstance();
            // if (app.GetDeviceState() == kDeviceStateIdle) {
            //     app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            // }
        });
#endif
    }

    void InitializeSdcard() {
            esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,   // 如果挂载不成功是否需要格式化SD卡
            .max_files = 5, // 允许打开的最大文件数
            .allocation_unit_size = 16 * 1024  // 分配单元大小
        };
        
        sdmmc_card_t *card;
        const char mount_point[] = MOUNT_POINT;
        ESP_LOGD(TAG, "Initializing SD card");
        ESP_LOGD(TAG, "Using SDMMC peripheral");
    
        sdmmc_host_t host = SDMMC_HOST_DEFAULT(); // SDMMC主机接口配置
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT(); // SDMMC插槽配置
        slot_config.width = 1;  // 设置为1线SD模式
        slot_config.clk = BSP_SD_CLK; 
        slot_config.cmd = BSP_SD_CMD;
        slot_config.d0 = BSP_SD_D0;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; // 打开内部上拉电阻
    
        // ESP_LOGI(TAG, "Mounting filesystem");
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card); // 挂载SD卡
    
        if (ret != ESP_OK) {  // 如果没有挂载成功
            if (ret == ESP_FAIL) { // 如果挂载失败
                ESP_LOGE(TAG, "Failed to mount filesystem. ");
            } else { // 如果是其它错误 打印错误名称
                ESP_LOGE(TAG, "Failed to initialize the card (%s). ", esp_err_to_name(ret));
            }
            return;
        }
        ESP_LOGI(TAG, "Filesystem mounted"); // 提示挂载成功
        sdmmc_card_print_info(stdout, card); // 终端打印SD卡的一些信息
        
    }

    void InitializeBatteryMonitor() {
        bat_monitor_config_t config = {
        .adc_ch = ADC_CHANNEL_6,     // ADC通道6
        .charge_io = GPIO_NUM_NC,    // 充电检测IO引脚，如不使用配置为-1
        .v_div_ratio = 1.0f,         // 电压分压比
        .v_min = 2.0f,               // 电池亏点电压2.0V
        .v_max = 3.7f,               // 电池满电电压3.7V
        .low_thresh = 10.0f,         // 低电量阈值10%
        .report_ms = 5000            // 5秒报告间隔
        };
        bat_monitor_handle_t handle = bat_monitor_create(&config);
        if (!handle) {
            ESP_LOGE(TAG, "电池监测初始化失败");
            return;
        }
        
        // 设置事件回调
        bat_monitor_set_event_cb(handle, 
                    [](bat_monitor_event_t event, float voltage, void *user_data) {
                    switch (event) {
                        case BAT_EVENT_VOLTAGE_REPORT:
                            ESP_LOGI(TAG, "电池电压: %.2fV", voltage);
                            break;
                        case BAT_EVENT_FULL:
                            ESP_LOGI(TAG, "电池已充满 (%.2fV)", voltage);
                            break;
                        case BAT_EVENT_LOW:
                            ESP_LOGI(TAG, "电池电量低 (%.2fV)", voltage);
                            break;
                        case BAT_EVENT_CHARGING_BEGIN:
                            ESP_LOGI(TAG, "开始充电");
                            break;
                        case BAT_EVENT_CHARGING_STOP:
                            ESP_LOGI(TAG, "停止充电");
                            break;
                    }
                }, NULL);
        ESP_LOGI(TAG, "电池监测已启动");
    };

public:
    LichuangDevBoard() : boot_button_Boot_IO0(BOOT_BUTTON_GPIO),
    #if my                     
    boot_button_IO6(GPIO_NUM_0)
    #else
    boot_button_IO6(GPIO_NUM_6)
    #endif
    {
        InitializeI2c();
        InitializeSpi();
        InitializeSdcard();
        InitializeButtons();
        #if !my
        // InitializeBatteryMonitor();
        #else
        #endif
        GetBacklight()->RestoreBrightness();
        RC522_Init();
        RC522_Rese( );//复位RC522
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
#endif
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(LichuangDevBoard);
