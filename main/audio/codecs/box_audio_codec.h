#ifndef _BOX_AUDIO_CODEC_H
#define _BOX_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>
/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 *   ES8311_REGISTER NAME_REG_REGISTER ADDRESS
 */
#define ES8311_RESET_REG00       0x00 /*reset digital,csm,clock manager etc.*/

/*
 * Clock Scheme Register definition
 */
#define ES8311_CLK_MANAGER_REG01 0x01 /* select clk src for mclk, enable clock for codec */
#define ES8311_CLK_MANAGER_REG02 0x02 /* clk divider and clk multiplier */
#define ES8311_CLK_MANAGER_REG03 0x03 /* adc fsmode and osr  */
#define ES8311_CLK_MANAGER_REG04 0x04 /* dac osr */
#define ES8311_CLK_MANAGER_REG05 0x05 /* clk divier for adc and dac */
#define ES8311_CLK_MANAGER_REG06 0x06 /* bclk inverter and divider */
#define ES8311_CLK_MANAGER_REG07 0x07 /* tri-state, lrck divider */
#define ES8311_CLK_MANAGER_REG08 0x08 /* lrck divider */
/*
 * SDP
 */
#define ES8311_SDPIN_REG09       0x09 /* dac serial digital port */
#define ES8311_SDPOUT_REG0A      0x0A /* adc serial digital port */
/*
 * SYSTEM
 */
#define ES8311_SYSTEM_REG0B      0x0B /* system */
#define ES8311_SYSTEM_REG0C      0x0C /* system */
#define ES8311_SYSTEM_REG0D      0x0D /* system, power up/down */
#define ES8311_SYSTEM_REG0E      0x0E /* system, power up/down */
#define ES8311_SYSTEM_REG0F      0x0F /* system, low power */
#define ES8311_SYSTEM_REG10      0x10 /* system */
#define ES8311_SYSTEM_REG11      0x11 /* system */
#define ES8311_SYSTEM_REG12      0x12 /* system, Enable DAC */
#define ES8311_SYSTEM_REG13      0x13 /* system */
#define ES8311_SYSTEM_REG14      0x14 /* system, select DMIC, select analog pga gain */
/*
 * ADC
 */
#define ES8311_ADC_REG15         0x15 /* ADC, adc ramp rate, dmic sense */
#define ES8311_ADC_REG16         0x16 /* ADC */
#define ES8311_ADC_REG17         0x17 /* ADC, volume */
#define ES8311_ADC_REG18         0x18 /* ADC, alc enable and winsize */
#define ES8311_ADC_REG19         0x19 /* ADC, alc maxlevel */
#define ES8311_ADC_REG1A         0x1A /* ADC, alc automute */
#define ES8311_ADC_REG1B         0x1B /* ADC, alc automute, adc hpf s1 */
#define ES8311_ADC_REG1C         0x1C /* ADC, equalizer, hpf s2 */
/*
 * DAC
 */
#define ES8311_DAC_REG31         0x31 /* DAC, mute */
#define ES8311_DAC_REG32         0x32 /* DAC, volume */
#define ES8311_DAC_REG33         0x33 /* DAC, offset */
#define ES8311_DAC_REG34         0x34 /* DAC, drc enable, drc winsize */
#define ES8311_DAC_REG35         0x35 /* DAC, drc maxlevel, minilevel */
#define ES8311_DAC_REG37         0x37 /* DAC, ramprate */
/*
 *GPIO
 */
#define ES8311_GPIO_REG44        0x44 /* GPIO, dac2adc for test */
#define ES8311_GP_REG45          0x45 /* GP CONTROL */
/*
 * CHIP
 */
#define ES8311_CHD1_REGFD        0xFD /* CHIP ID1 */
#define ES8311_CHD2_REGFE        0xFE /* CHIP ID2 */
#define ES8311_CHVER_REGFF       0xFF /* VERSION */
#define ES8311_CHD1_REGFD        0xFD /* CHIP ID1 */

#define ES8311_MAX_REGISTER      0xFF

typedef enum {
    ES8311_MIC_GAIN_MIN = -1,
    ES8311_MIC_GAIN_0DB,
    ES8311_MIC_GAIN_6DB,
    ES8311_MIC_GAIN_12DB,
    ES8311_MIC_GAIN_18DB,
    ES8311_MIC_GAIN_24DB,
    ES8311_MIC_GAIN_30DB,
    ES8311_MIC_GAIN_36DB,
    ES8311_MIC_GAIN_42DB,
    ES8311_MIC_GAIN_MAX
} es8311_mic_gain_t;




class BoxAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
public:
    BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference);
    virtual ~BoxAudioCodec();
    virtual void Shutdown() override;
    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    int es8311_write_reg(int reg, int value);
    int es8311_read_reg(int reg, int *value);
    int es7210_write_reg(int reg, int value);
    int es7210_read_reg(int reg, int *value);
    bool es8311_verify_low_power();
    int es8311_enter_minimum_power_mode();
    bool es7210_verify_low_power();
    int es7210_enter_minimum_power_mode();
};
#define ES7210_RESET_REG00           0x00 /* Reset control */
#define ES7210_CLOCK_OFF_REG01       0x01 /* Used to turn off the ADC clock */
#define ES7210_MAINCLK_REG02         0x02 /* Set ADC clock frequency division */
#define ES7210_MASTER_CLK_REG03      0x03 /* MCLK source $ SCLK division */
#define ES7210_LRCK_DIVH_REG04       0x04 /* lrck_divh */
#define ES7210_LRCK_DIVL_REG05       0x05 /* lrck_divl */
#define ES7210_POWER_DOWN_REG06      0x06 /* power down */
#define ES7210_OSR_REG07             0x07
#define ES7210_MODE_CONFIG_REG08     0x08 /* Set master/slave & channels */
#define ES7210_TIME_CONTROL0_REG09   0x09 /* Set Chip intial state period*/
#define ES7210_TIME_CONTROL1_REG0A   0x0A /* Set Power up state period */
#define ES7210_SDP_INTERFACE1_REG11  0x11 /* Set sample & fmt */
#define ES7210_SDP_INTERFACE2_REG12  0x12 /* Pins state */
#define ES7210_ADC_AUTOMUTE_REG13    0x13 /* Set mute */
#define ES7210_ADC34_MUTERANGE_REG14 0x14 /* Set mute range */
#define ES7210_ADC34_HPF2_REG20      0x20 /* HPF */
#define ES7210_ADC34_HPF1_REG21      0x21
#define ES7210_ADC12_HPF1_REG22      0x22
#define ES7210_ADC12_HPF2_REG23      0x23
#define ES7210_ANALOG_REG40          0x40 /* ANALOG Power */
#define ES7210_MIC12_BIAS_REG41      0x41
#define ES7210_MIC34_BIAS_REG42      0x42
#define ES7210_MIC1_GAIN_REG43       0x43
#define ES7210_MIC2_GAIN_REG44       0x44
#define ES7210_MIC3_GAIN_REG45       0x45
#define ES7210_MIC4_GAIN_REG46       0x46
#define ES7210_MIC1_POWER_REG47      0x47
#define ES7210_MIC2_POWER_REG48      0x48
#define ES7210_MIC3_POWER_REG49      0x49
#define ES7210_MIC4_POWER_REG4A      0x4A
#define ES7210_MIC12_POWER_REG4B     0x4B /* MICBias & ADC & PGA Power */
#define ES7210_MIC34_POWER_REG4C     0x4C

typedef enum {
    ES7210_AD1_AD0_00 = 0x80,
    ES7210_AD1_AD0_01 = 0x82,
    ES7210_AD1_AD0_10 = 0x84,
    ES7210_AD1_AD0_11 = 0x86
} es7210_address_t;

typedef enum {
    ES7210_INPUT_MIC1 = 0x01,
    ES7210_INPUT_MIC2 = 0x02,
    ES7210_INPUT_MIC3 = 0x04,
    ES7210_INPUT_MIC4 = 0x08
} es7210_input_mics_t;

typedef enum gain_value {
    GAIN_0DB = 0,
    GAIN_3DB,
    GAIN_6DB,
    GAIN_9DB,
    GAIN_12DB,
    GAIN_15DB,
    GAIN_18DB,
    GAIN_21DB,
    GAIN_24DB,
    GAIN_27DB,
    GAIN_30DB,
    GAIN_33DB,
    GAIN_34_5DB,
    GAIN_36DB,
    GAIN_37_5DB,
} es7210_gain_value_t;

#endif // _BOX_AUDIO_CODEC_H