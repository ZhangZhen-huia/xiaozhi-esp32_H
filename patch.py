import re

with open('main/audio/codecs/box_audio_codec.h', 'r') as f:
    text = f.read()

text = text.replace('const audio_codec_data_if_t* data_if_ = nullptr;', 'const audio_codec_data_if_t* in_data_if_ = nullptr;\n    const audio_codec_data_if_t* out_data_if_ = nullptr;')
with open('main/audio/codecs/box_audio_codec.h', 'w') as f:
    f.write(text)

with open('main/audio/codecs/box_audio_codec.cc', 'r') as f:
    text = f.read()

rep = """    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg_out = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = tx_handle_,
    };
    out_data_if_ = audio_codec_new_i2s_data(&i2s_cfg_out);
    assert(out_data_if_ != NULL);

    audio_codec_i2s_cfg_t i2s_cfg_in = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = NULL,
    };
    in_data_if_ = audio_codec_new_i2s_data(&i2s_cfg_in);
    assert(in_data_if_ != NULL);"""

old = """    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);"""

text = text.replace(old, rep)
text = text.replace('.data_if = data_if_,', '.data_if = out_data_if_,', 1)

old_dev = """    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);"""
new_dev = """    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    dev_cfg.data_if = in_data_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);"""
text = text.replace(old_dev, new_dev)
text = text.replace('audio_codec_delete_data_if(data_if_);', 'audio_codec_delete_data_if(in_data_if_);\n    audio_codec_delete_data_if(out_data_if_);')

with open('main/audio/codecs/box_audio_codec.cc', 'w') as f:
    f.write(text)

