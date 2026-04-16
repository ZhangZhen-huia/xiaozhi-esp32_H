import re

with open('main/audio/codecs/box_audio_codec.cc', 'r') as f:
    text = f.read()

old_read = "int BoxAudioCodec::Read(int16_t* dest, int samples) {\n    if (input_enabled_) {"
new_read = "int BoxAudioCodec::Read(int16_t* dest, int samples) {\n    std::lock_guard<std::mutex> lock(data_if_mutex_);\n    if (input_enabled_) {"

old_write = "int BoxAudioCodec::Write(const int16_t* data, int samples) {\n    if (output_enabled_) {"
new_write = "int BoxAudioCodec::Write(const int16_t* data, int samples) {\n    std::lock_guard<std::mutex> lock(data_if_mutex_);\n    if (output_enabled_) {"

text = text.replace(old_read, new_read)
text = text.replace(old_write, new_write)

with open('main/audio/codecs/box_audio_codec.cc', 'w') as f:
    f.write(text)

