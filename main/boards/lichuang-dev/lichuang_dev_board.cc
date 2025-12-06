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
#include "adc_battery_monitor.h"
#include "esp32_rc522.h"

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
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;
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
        // gpio_config_t io11_conf;
        // io11_conf.mode = GPIO_MODE_OUTPUT;
        // io11_conf.pin_bit_mask = 11<<GPIO_NUM_0;

        // // 根据上面的配置 设置GPIO
        // gpio_config(&io11_conf);
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

        //IO6按键
        boot_button_IO6.OnClick([this]() {
            auto& app = Application::GetInstance();
        
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
        //    
            auto music = Board::GetMusic();
            // music->StopStreaming();
            std::string msg = "播放下一首";
            // app.SetDeviceState(kDeviceStateIdle);
            app.SendMessage(msg);
            
        });
        
        // boot_button_IO6.OnLongPress([this]() {
        //     auto& app = Application::GetInstance();
        //     // ResetWifiConfiguration();
        //     app.ToggleChatState();
        // });


        boot_button_Boot_IO0.OnClick([this](){
            auto& app = Application::GetInstance();
            app.ToggleChatState();
        });
        //Boot按键
        boot_button_Boot_IO0.OnLongPressStart([this](){
            auto& app = Application::GetInstance();
            app.StartListening();
        });
        boot_button_Boot_IO0.OnPressUp([this](){
            auto& app = Application::GetInstance();
            app.StopListening();
        });
#if CONFIG_USE_DEVICE_AEC
        boot_button_Boot_IO0.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
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
        // ESP_LOGI(TAG, "Initializing SD card");
        // ESP_LOGI(TAG, "Using SDMMC peripheral");
    
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
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_6, 680000, 680000, GPIO_NUM_6);
        adc_battery_monitor_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                // sleep_timer_->SetEnabled(false);
            } else {
                // sleep_timer_->SetEnabled(true);
            }
        });
    }

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
        InitializeBatteryMonitor();
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
    virtual AdcBatteryMonitor* GetBatteryMonitor() override {
        return adc_battery_monitor_;
    }
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging)override{
        level = adc_battery_monitor_->GetBatteryLevel();
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        return true;
    };
};

DECLARE_BOARD(LichuangDevBoard);
