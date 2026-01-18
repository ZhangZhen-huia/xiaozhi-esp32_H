#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "esp_sleep.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // // 1. 首先初始化日志系统（如果需要）
    // esp_log_level_set("*", ESP_LOG_INFO);
    
    // // 2. 配置唤醒源
    esp_err_t rc = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    // if (rc != ESP_OK) {
    //     ESP_LOGE("MAIN", "esp_sleep_enable_ext0_wakeup 返回 %d", rc);
    // }

    // // 3. 配置所有GPIO为低功耗状态
    // ESP_LOGI("MAIN", "配置所有GPIO为低功耗状态");
    
    // // ESP32-S3的有效GPIO范围
    // int valid_gpios[] = {
    //     0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
    //     20,21,34,35,36,37,38,39,
    //     40,41,42,43,44,45,46,47,48
    // };
    
    // for (int i = 0; i < sizeof(valid_gpios)/sizeof(valid_gpios[0]); i++) {
    //     gpio_num_t gpio = (gpio_num_t)valid_gpios[i];
        
    //     // 跳过唤醒引脚（GPIO0）
    //     if (gpio == GPIO_NUM_0) continue;
        
    //     // 重置GPIO
    //     gpio_reset_pin(gpio);
        
    //     // 配置为输入模式，禁用上下拉
    //     gpio_config_t io_conf = {};
    //     io_conf.pin_bit_mask = (1ULL << gpio);
    //     io_conf.mode = GPIO_MODE_INPUT;
    //     io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    //     io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //     io_conf.intr_type = GPIO_INTR_DISABLE;
    //     gpio_config(&io_conf);
    // }
    
    // // 4. 配置电源域（ESP-IDF 5.5.1版本）
    // // 在ESP-IDF 5.x中，电源域配置有所不同
    // ESP_LOGI("MAIN", "配置电源域");
    
    // // 查看当前ESP-IDF版本支持的电源域
    // #ifdef ESP_PD_DOMAIN_RTC_PERIPH
    // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    // #endif
    
    // #ifdef ESP_PD_DOMAIN_XTAL
    // esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
    // #endif
    
    // #ifdef ESP_PD_DOMAIN_VDDSDIO
    // esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF);
    // #endif
    
    // // 5. 禁用所有GPIO保持功能
    // gpio_deep_sleep_hold_dis();  // 全局禁用深度睡眠保持
    
    // // 6. 短暂延迟后进入深度睡眠
    // vTaskDelay(pdMS_TO_TICKS(100));
    
    // ESP_LOGI("MAIN", "=============进入深度睡眠===============");
    // ESP_LOGI("MAIN", "如果此时电流为2mA，说明问题在应用初始化代码中");
    
    // // 进入深度睡眠
    // esp_deep_sleep_start();
    // Initialize the default event loop
    
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
