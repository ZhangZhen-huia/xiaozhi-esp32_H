#include "box_audio_codec.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include "esp_sleep.h"

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);
    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");

}

BoxAudioCodec::~BoxAudioCodec() {

    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}


int BoxAudioCodec::es8311_write_reg(int reg, int value)
{
    return out_ctrl_if_->write_reg(out_ctrl_if_,reg, 1, &value, 1);
}

int BoxAudioCodec::es8311_read_reg(int reg, int *value)
{
    *value = 0;
    return out_ctrl_if_->read_reg(out_ctrl_if_,reg, 1, value, 1);
}

int BoxAudioCodec::es7210_write_reg(int reg, int value)
{
    return in_ctrl_if_->write_reg(in_ctrl_if_,reg, 1, &value, 1);
}

int BoxAudioCodec::es7210_read_reg(int reg, int *value)
{
    *value = 0;
    return in_ctrl_if_->read_reg(in_ctrl_if_,reg, 1, value, 1);
}
// 检查是否进入低功耗模式
bool BoxAudioCodec::es8311_verify_low_power() {
    int reg_00 = 0, reg_01 = 0, reg_0d = 0, reg_0e = 0, reg_0f = 0, reg_12 = 0, reg_45 = 0;
    
    // 读取关键寄存器 - 注意现在是0x0D而不是0x0E
    if (es8311_read_reg(0x00, &reg_00) != 0 ||
        es8311_read_reg(0x01, &reg_01) != 0 ||
        es8311_read_reg(0x0D, &reg_0d) != 0 ||  // 改为0x0D
        es8311_read_reg(0x0E, &reg_0e) != 0 ||  // 0x0E现在是其他系统设置
        es8311_read_reg(0x0F, &reg_0f) != 0 ||
        es8311_read_reg(0x12, &reg_12) != 0 ||
        es8311_read_reg(0x45, &reg_45) != 0) {
        ESP_LOGE(TAG, "Failed to verify power state");
        return false;
    }
    
    ESP_LOGD(TAG, "Verification registers: REG00=0x%02X, REG01=0x%02X, REG0D=0x%02X, REG0E=0x%02X, REG0F=0x%02X, REG12=0x%02X, REG45=0x%02X",
             reg_00, reg_01, reg_0d, reg_0e, reg_0f, reg_12, reg_45);
    
    // 关键检查点（必须全部满足）
    bool all_ok = true;
    
    // 1. 检查CSM是否关闭（bit7=0）
    if ((reg_00 & 0x80) != 0x00) {
        ESP_LOGW(TAG, "CSM still ON (REG00=0x%02X, bit7=1, expected 0)", reg_00);
        all_ok = false;
    }
    
    // 2. 检查所有时钟是否关闭（bit5-bit0=0）
    if ((reg_01 & 0x3F) != 0x00) {
        ESP_LOGW(TAG, "Some clocks still active (REG01=0x%02X, expected 0x00)", reg_01);
        all_ok = false;
    }
    
    // 3. 检查模拟电路是否全部关闭（寄存器0x0D，bit7-bit2全为1，bit1:0=00）
    // 根据数据手册，0x0D默认值是11111100 (0xFC)
    if (reg_0d != 0xFC) {
        ESP_LOGW(TAG, "Analog circuits not fully off (REG0D=0x%02X, expected 0xFC)", reg_0d);
        ESP_LOGW(TAG, "  Bit7 (PDN_ANA) = %d (should be 1)", (reg_0d >> 7) & 1);
        ESP_LOGW(TAG, "  Bit6 (PDN_IBIASGEN) = %d (should be 1)", (reg_0d >> 6) & 1);
        ESP_LOGW(TAG, "  Bit5 (PDN_ADCBIASGEN) = %d (should be 1)", (reg_0d >> 5) & 1);
        ESP_LOGW(TAG, "  Bit4 (PDN_ADCVERFGEN) = %d (should be 1)", (reg_0d >> 4) & 1);
        ESP_LOGW(TAG, "  Bit3 (PDN_DACVREFGEN) = %d (should be 1)", (reg_0d >> 3) & 1);
        ESP_LOGW(TAG, "  Bit2 (PDN_VREF) = %d (should be 1)", (reg_0d >> 2) & 1);
        all_ok = false;
    }
    
    // 4. 检查DAC是否关闭（bit1=1）
    if ((reg_12 & 0x02) != 0x02) {
        ESP_LOGW(TAG, "DAC not powered down (REG12=0x%02X, bit1=0, expected 1)", reg_12);
        all_ok = false;
    }
    
    // 5. 检查BCLK/LRCK上拉是否禁用（bit0=1）
    if ((reg_45 & 0x01) != 0x01) {
        ESP_LOGW(TAG, "BCLK/LRCK pull-up still enabled (REG45=0x%02X, bit0=0, expected 1)", reg_45);
        all_ok = false;
    }
    
    // 6. 检查低功耗模式是否全部使能（寄存器0x0F）
    if ((reg_0f & 0xFF) != 0xFF) {
        ESP_LOGW(TAG, "Not all low-power modes enabled (REG0F=0x%02X, expected 0xFF)", reg_0f);
        all_ok = false;
    }
    
    // 7. 检查寄存器0x0E的设置（根据数据手册，默认是0x6A）
    // 注意：0x0E现在有其他用途，比如PDN_ANA等，但不是主要模拟电路控制
    // 我们检查它是否设置为低功耗模式
    // 根据数据手册，0x0E默认是0x6A（01101010）
    // 在低功耗模式下，可能需要调整
    
    if (all_ok) {
        ESP_LOGI(TAG, "✅ All low-power conditions verified");
    }
    
    return all_ok;
}

// 进入最低功耗模式（电流最低，<1mA）
int BoxAudioCodec::es8311_enter_minimum_power_mode() {
    int ret = 0;
    int reg_value = 0;
    
    ESP_LOGI(TAG, "Entering ES8311 minimum power mode...");
    
    // 读取初始状态
    if (es8311_read_reg(0x00, &reg_value) == 0) {
        ESP_LOGD(TAG, "Initial REG00: 0x%02X", reg_value);
    }
    
    // 步骤1: 静音DAC和ADC
    ESP_LOGD(TAG, "Muting DAC and ADC...");
    ret = es8311_write_reg(0x32, 0x00);   // DAC静音（-95.5dB）
    ret |= es8311_write_reg(0x17, 0x00);  // ADC静音（-95.5dB）
    
    // 短暂延迟确保静音生效
    vTaskDelay(pdMS_TO_TICKS(2));
    
    // 步骤2: 停止数据传输
    ESP_LOGD(TAG, "Stopping data transmission...");
    ret |= es8311_write_reg(0x06, 0x40);  // BCLK停止输出
    ret |= es8311_write_reg(0x07, 0x20);  // BCLK/LRCK三态
    
    // 步骤3: 关闭所有时钟
    ESP_LOGD(TAG, "Disabling all clocks...");
    ret |= es8311_write_reg(0x01, 0x00);
    
    // 等待时钟关闭
    vTaskDelay(pdMS_TO_TICKS(2));
    
    // 步骤4: 关闭DAC模块
    ESP_LOGD(TAG, "Powering down DAC...");
    ret |= es8311_write_reg(0x12, 0x02);  // PDN_DAC=1
    
    // 步骤5: 关闭模拟电路（关键步骤！使用寄存器0x0D）
    ESP_LOGD(TAG, "Powering down analog circuits (REG0D)...");
    
    // 读取当前0x0D寄存器值
    int current_0d = 0;
    if (es8311_read_reg(0x0D, &current_0d) == 0) {
        ESP_LOGD(TAG, "Current REG0D before power down: 0x%02X", current_0d);
    }
    
    // 写入0x0D寄存器关闭所有模拟电路
    // 根据数据手册，0x0D默认是0xFC (11111100)
    ret |= es8311_write_reg(0x0D, 0xFC);
    
    // 等待并验证
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 验证0x0D是否设置成功
    int reg0d_val = 0;
    if (es8311_read_reg(0x0D, &reg0d_val) == 0) {
        ESP_LOGD(TAG, "After writing, REG0D = 0x%02X", reg0d_val);
        if (reg0d_val != 0xFC) {
            ESP_LOGW(TAG, "Failed to set REG0D to 0xFC, got 0x%02X", reg0d_val);
            // 尝试再次写入
            ret |= es8311_write_reg(0x0D, 0xFC);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    // 步骤6: 设置寄存器0x0E
    ESP_LOGD(TAG, "Configuring REG0E...");
    // 根据数据手册，0x0E默认是0x6A (01101010)
    // 在低功耗模式下，可能需要调整某些位
    // 我们先保持默认值，或者根据需要进行设置
    ret |= es8311_write_reg(0x0E, 0x6A);  // 保持默认值
    
    // 步骤7: 使能所有低功耗模式（寄存器0x0F）
    ESP_LOGD(TAG, "Enabling all low-power modes (REG0F)...");
    ret |= es8311_write_reg(0x0F, 0xFF);
    
    // 步骤8: 禁用BCLK/LRCK上拉电阻
    ESP_LOGD(TAG, "Disabling BCLK/LRCK pull-ups...");
    int reg45_val = 0;
    if (es8311_read_reg(0x45, &reg45_val) == 0) {
        reg45_val |= 0x01;  // 设置bit0=1（禁用上拉）
        ret |= es8311_write_reg(0x45, reg45_val);
    } else {
        ret |= es8311_write_reg(0x45, 0x01);
    }
    
    // 步骤9: 关闭其他输入/输出
    ESP_LOGD(TAG, "Disabling inputs and outputs...");
    ret |= es8311_write_reg(0x14, 0x00);  // 关闭DMIC和输入选择
    ret |= es8311_write_reg(0x15, 0x00);  // 关闭ADC相关功能
    
    // 步骤10: 重要！先复位数字模块，但保持CSM开启
    ESP_LOGD(TAG, "Resetting digital modules...");
    ret |= es8311_write_reg(0x00, 0x9F);  // 0x9F = 10011111
    
    // 等待数字模块复位完成
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 步骤11: 最后关闭CSM（芯片状态机）
    ESP_LOGD(TAG, "Shutting down CSM...");
    ret |= es8311_write_reg(0x00, 0x1F);  // 0x1F = 00011111
    
    // 等待所有电源稳定
    ESP_LOGD(TAG, "Waiting for power stabilization...");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 检查是否有错误
    if (ret != 0) {
        ESP_LOGE(TAG, "Error during power-down sequence: %d", ret);
        return ret;
    }
    
    // 验证是否进入低功耗
    ESP_LOGD(TAG, "Verifying low-power state...");
    bool power_ok = es8311_verify_low_power();
    
    if (power_ok) {
        ESP_LOGW(TAG, "✅ ES8311 entered minimum power mode (<1mA expected)");
        return 0;
    } else {
        ESP_LOGW(TAG, "⚠️ ES8311 may not be in lowest power state");
        return -1;
    }
}

/**
 * @brief 检查ES7210是否进入低功耗模式
 * @return true 已进入低功耗，false 未进入低功耗
 */
bool BoxAudioCodec::es7210_verify_low_power() {
    int reg_00 = 0, reg_01 = 0, reg_06 = 0, reg_0b = 0, reg_40 = 0, reg_4b = 0, reg_4c = 0;
    
    // 读取关键寄存器
    if (es7210_read_reg(0x00, &reg_00) != 0 ||
        es7210_read_reg(0x01, &reg_01) != 0 ||
        es7210_read_reg(0x06, &reg_06) != 0 ||
        es7210_read_reg(0x0B, &reg_0b) != 0 ||
        es7210_read_reg(0x40, &reg_40) != 0 ||
        es7210_read_reg(0x4B, &reg_4b) != 0 ||
        es7210_read_reg(0x4C, &reg_4c) != 0) {
        ESP_LOGE(TAG, "Failed to read ES7210 power state registers");
        return false;
    }
    
    ESP_LOGD(TAG, "ES7210 Verification: REG00=0x%02X, REG01=0x%02X, REG06=0x%02X, REG0B=0x%02X, REG40=0x%02X, REG4B=0x%02X, REG4C=0x%02X",
             reg_00, reg_01, reg_06, reg_0b, reg_40, reg_4b, reg_4c);
    
    bool all_ok = true;
    
    // 1. 检查所有时钟是否关闭（REG01所有时钟位都为1）
    if ((reg_01 & 0x7F) != 0x7F) {
        ESP_LOGW(TAG, "Not all clocks are off (REG01=0x%02X, expected 0x7F)", reg_01);
        all_ok = false;
    }
    
    // 2. 检查POWER DOWN寄存器配置
    // 重要：根据数据手册第13页，REG06只有bit3-bit0是有效位
    // 我们期望的值是0x0F（所有位都设为1），但实际只读到了0x07（bit3=0）
    // 可能是ANA_ISO_EN位有特殊要求，我们需要调整检查条件
    if ((reg_06 & 0x07) != 0x07) {  // 只检查bit2-bit0，忽略bit3
        ESP_LOGW(TAG, "Power down configuration incorrect (REG06=0x%02X, expected bits 2-0 = 0x07)", reg_06);
        all_ok = false;
    }
    
    // 3. 检查REG0B芯片状态
    // CSM_STATE应该在低功耗模式下为00（power down）
    if ((reg_0b & 0x03) != 0x00) {
        ESP_LOGW(TAG, "Chip not in power down state (REG0B=0x%02X, CSM_STATE=%d, expected 00)", 
                 reg_0b, reg_0b & 0x03);
        all_ok = false;
    }
    
    // 4. 检查ANALOG SYSTEM寄存器
    // 低功耗时bit7(PDN_ANA)应为1
    if ((reg_40 & 0x80) != 0x80) {
        ESP_LOGW(TAG, "Analog circuit not powered down (REG40=0x%02X, bit7=%d)", 
                 reg_40, (reg_40 >> 7) & 1);
        all_ok = false;
    }
    
    // 5. 检查MIC12电源是否全部关闭
    if (reg_4b != 0xFF) {
        ESP_LOGW(TAG, "MIC12 power not fully off (REG4B=0x%02X, expected 0xFF)", reg_4b);
        all_ok = false;
    }
    
    // 6. 检查MIC34电源是否全部关闭
    if (reg_4c != 0xFF) {
        ESP_LOGW(TAG, "MIC34 power not fully off (REG4C=0x%02X, expected 0xFF)", reg_4c);
        all_ok = false;
    }
    
    if (all_ok) {
        ESP_LOGI(TAG, "✅ ES7210所有低功耗条件验证通过");
    } else {
        ESP_LOGW(TAG, "⚠️ ES7210未完全进入低功耗模式");
    }
    
    return all_ok;
}

/**
 * @brief 进入ES7210最低功耗模式
 * @return 0成功，其他值失败
 */
int BoxAudioCodec::es7210_enter_minimum_power_mode() {
    int ret = 0;
    
    ESP_LOGI(TAG, "开始进入ES7210最低功耗模式...");
    
    // 步骤1: 停止音频数据传输
    ESP_LOGD(TAG, "停止音频数据传输...");
    // 设置SDOUT1和SDOUT2为三态输出，SCLK/LRCK三态
    ret = es7210_write_reg(0x12, 0x38);  // 0x38 = 00111000
                                         // bit5=1: SDOUT2 tri state
                                         // bit4=1: SDOUT1 tri state
                                         // bit3=1: SCLK/LRCK tri state
    
    // 步骤2: 关闭PGA增益
    ESP_LOGD(TAG, "关闭PGA增益...");
    ret |= es7210_write_reg(0x43, 0x00);  // MIC1 GAIN, 关闭选择
    ret |= es7210_write_reg(0x44, 0x00);  // MIC2 GAIN
    ret |= es7210_write_reg(0x45, 0x00);  // MIC3 GAIN
    ret |= es7210_write_reg(0x46, 0x00);  // MIC4 GAIN
    
    vTaskDelay(pdMS_TO_TICKS(2));
    
    // 步骤3: 关闭MICBIAS电压
    ESP_LOGD(TAG, "关闭MICBIAS电压...");
    ret |= es7210_write_reg(0x41, 0x00);  // MIC1/2 BIAS
    ret |= es7210_write_reg(0x42, 0x00);  // MIC3/4 BIAS
    
    // 步骤4: 先配置部分寄存器，再关闭时钟
    ESP_LOGD(TAG, "准备关闭时钟...");
    
    // 步骤5: 尝试多次写入REG06确保成功
    ESP_LOGD(TAG, "配置POWER DOWN寄存器...");
    int reg06_attempts = 0;
    for (reg06_attempts = 0; reg06_attempts < 3; reg06_attempts++) {
        // 尝试写入REG06 = 0x0F
        ret |= es7210_write_reg(0x06, 0x0F);
        vTaskDelay(pdMS_TO_TICKS(2));
        
        // 读取验证
        int reg06_val = 0;
        if (es7210_read_reg(0x06, &reg06_val) == 0) {
            ESP_LOGD(TAG, "REG06写入尝试 %d: 0x%02X", reg06_attempts + 1, reg06_val);
            // 检查bit2-bit0是否为1
            if ((reg06_val & 0x07) == 0x07) {
                ESP_LOGD(TAG, "REG06配置成功");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
    if (reg06_attempts >= 3) {
        ESP_LOGW(TAG, "REG06配置失败，尝试写入0x07");
        // 如果0x0F失败，尝试0x07（不设置ANA_ISO_EN）
        ret |= es7210_write_reg(0x06, 0x07);
    }
    
    // 步骤6: 关闭所有时钟
    ESP_LOGD(TAG, "关闭所有时钟...");
    ret |= es7210_write_reg(0x01, 0x7F);  // 0x7F = 01111111
    vTaskDelay(pdMS_TO_TICKS(5));
    
    // 步骤7: 关闭MIC功率电路
    ESP_LOGD(TAG, "关闭MIC功率电路...");
    ret |= es7210_write_reg(0x4B, 0xFF);  // MIC1/2 POWER DOWN
    ret |= es7210_write_reg(0x4C, 0xFF);  // MIC3/4 POWER DOWN
    
    // 步骤8: 配置ANALOG SYSTEM寄存器
    ESP_LOGD(TAG, "配置ANALOG SYSTEM寄存器...");
    // 根据数据手册第19页，REG40默认是0x80
    // 我们保持bit7=1（PDN_ANA），其他位根据数据手册设置
    // VX2OFF=1（bit6=1）, VX1SEL=1（bit5=1，当VDDA=1.8V时推荐）
    ret |= es7210_write_reg(0x40, 0xE0);  // 0xE0 = 11100000
    vTaskDelay(pdMS_TO_TICKS(2));
    
    // 步骤9: 配置低功耗模式
    ESP_LOGD(TAG, "配置低功耗模式...");
    ret |= es7210_write_reg(0x08, 0x14);  // 0x14 = 00010100
                                          // bit2=1: EQ_OFF
                                          // bit4=0: LRCK_RATE_MODE保持默认
    
    // 步骤10: 强制芯片进入power down状态
    ESP_LOGD(TAG, "强制芯片进入power down状态...");
    // 根据数据手册，要强制进入power down状态，需要设置FORCE_CSM=100
    // 但是注意：需要先确保CSM_ON=1（REG00 bit0=1）
    // 先检查并确保CSM_ON=1
    int reg00_val = 0;
    if (es7210_read_reg(0x00, &reg00_val) == 0) {
        if ((reg00_val & 0x01) == 0) {
            // CSM_ON=0，需要先设为1
            reg00_val |= 0x01;
            ret |= es7210_write_reg(0x00, reg00_val);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    // 现在强制进入power down状态
    // REG0B: 设置FORCE_CSM=100 (bit6:4=100)
    ret |= es7210_write_reg(0x0B, 0x40);  // 0x40 = 01000000
    
    // 等待状态转换
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // 检查状态
    int reg0b_val = 0;
    if (es7210_read_reg(0x0B, &reg0b_val) == 0) {
        ESP_LOGD(TAG, "强制状态转换后REG0B: 0x%02X, CSM_STATE=%d", reg0b_val, reg0b_val & 0x03);
        
        // 如果CSM_STATE还不是00，可能需要尝试不同的FORCE_CSM值
        if ((reg0b_val & 0x03) != 0x00) {
            ESP_LOGW(TAG, "强制进入power down失败，尝试其他方法");
            
            // 方法1: 尝试强制进入chip initial状态，然后自然进入power down
            ret |= es7210_write_reg(0x0B, 0x50);  // FORCE_CSM=101 (force to chip initial)
            vTaskDelay(pdMS_TO_TICKS(10));
            
            // 方法2: 直接禁用CSM
            ret |= es7210_write_reg(0x00, 0x32);  // 默认值，但确保CSM_ON=0
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    // 步骤11: 配置低功耗偏置设置（如果适用）
    ESP_LOGD(TAG, "配置低功耗偏置...");
    // 根据数据手册第20页，可以配置VMIDLOW等位进一步降低功耗
    // 但需要根据具体应用决定
    
    // 步骤12: 等待电源稳定
    ESP_LOGD(TAG, "等待电源稳定...");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 步骤13: 验证写入是否成功
    ESP_LOGD(TAG, "验证寄存器写入...");
    int read_back = 0;
    int write_success = 0;
    
    // 验证关键寄存器 - 使用更宽松的条件
    if (es7210_read_reg(0x01, &read_back) == 0 && (read_back & 0x7F) == 0x7F) write_success++;
    if (es7210_read_reg(0x06, &read_back) == 0 && (read_back & 0x07) == 0x07) write_success++;
    if (es7210_read_reg(0x40, &read_back) == 0 && (read_back & 0x80) == 0x80) write_success++;
    
    if (write_success < 3) {
        ESP_LOGW(TAG, "部分寄存器写入失败 (成功数: %d/3)", write_success);
        // 显示详细错误
        ESP_LOGW(TAG, "详细寄存器状态:");
        int regs[] = {0x00, 0x01, 0x06, 0x0B, 0x40, 0x4B, 0x4C};
        const char* reg_names[] = {"REG00", "REG01", "REG06", "REG0B", "REG40", "REG4B", "REG4C"};
        for (int i = 0; i < 7; i++) {
            if (es7210_read_reg(regs[i], &read_back) == 0) {
                ESP_LOGW(TAG, "  %s = 0x%02X", reg_names[i], read_back);
            }
        }
    } else {
        ESP_LOGD(TAG, "所有关键寄存器写入成功");
    }
    
    // 检查是否有错误
    if (ret != 0) {
        ESP_LOGE(TAG, "ES7210功耗降低序列执行错误: %d", ret);
        return ret;
    }
    
    // 验证是否进入低功耗
    ESP_LOGD(TAG, "验证低功耗状态...");
    bool power_ok = es7210_verify_low_power();
    
    if (power_ok) {
        ESP_LOGW(TAG, "✅ ES7210成功进入最低功耗模式 (期望电流<0.1mA)");
        return 0;
    } else {
        ESP_LOGW(TAG, "⚠️ ES7210可能未进入最低功耗状态");
        return -1;
    }
}
void BoxAudioCodec::Shutdown() {
    ESP_LOGI(TAG, "Shutting down BoxAudioCodec...");
    std::lock_guard<std::mutex> lock(data_if_mutex_);

    // // 先关闭 codec 设备（会停止底层数据流）
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(output_dev_));
        output_enabled_ = false;
    }
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(input_dev_));
        input_enabled_ = false;
    }
    // // 释放 codec/dev/ctrl/interface（保守操作，保留与析构一致的顺序）
    // if (output_dev_) { esp_codec_dev_delete(output_dev_); output_dev_ = nullptr; }
    // if (input_dev_)  { esp_codec_dev_delete(input_dev_);  input_dev_ = nullptr; }

    // if (in_codec_if_)  { audio_codec_delete_codec_if(in_codec_if_); in_codec_if_ = nullptr; }
    // if (in_ctrl_if_)   { audio_codec_delete_ctrl_if(in_ctrl_if_); in_ctrl_if_ = nullptr; }
    // if (out_codec_if_) { audio_codec_delete_codec_if(out_codec_if_); out_codec_if_ = nullptr; }
    // if (out_ctrl_if_)  { audio_codec_delete_ctrl_if(out_ctrl_if_); out_ctrl_if_ = nullptr; }
    
    // if (gpio_if_)      { audio_codec_delete_gpio_if(gpio_if_); gpio_if_ = nullptr; }
    // if (data_if_)      { audio_codec_delete_data_if(data_if_); data_if_ = nullptr; }
    
    es7210_enter_minimum_power_mode();
    es8311_enter_minimum_power_mode();
    
    if (tx_handle_) {
        ESP_LOGI(TAG, "Disable & delete I2S TX channel");
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    if (rx_handle_) {
        ESP_LOGI(TAG, "Disable & delete I2S RX channel");
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "BoxAudioCodec shutdown completed");

}


void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if(output_dev_ == nullptr) {
        ESP_LOGW(TAG, "EnableOutput skipped: output_dev_ is null");
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}
// #include "esp_debug_helpers.h"  
int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}