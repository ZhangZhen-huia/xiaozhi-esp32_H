#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include <esp_log.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include "dirent.h"
#include "stdio.h"
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <thread>   // 为线程ID比较
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "settings.h"
#include <queue>

#define TAG "Esp32Music"



Esp32Music::Esp32Music() : current_song_name_(),
                         is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false) {
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    InitializeMp3Decoder();
    event_group_ = xEventGroupCreate();
    xTaskCreate([](void* arg) {
        ((Esp32Music*)arg)->NextPlayTask(NULL);
        vTaskDelete(NULL);
    }, "next_play_task", 2048*3, this, 3, &NextPlay_task_handle_);

}

// 初始化 chunk 池，返回是否成功
bool Esp32Music::InitChunkPool(size_t count, size_t slot_size) {
    std::lock_guard<std::mutex> lk(chunk_pool_mutex_);
    if (!chunk_pool_all_.empty()) return true; // 已初始化
    if (count == 0 || slot_size == 0) return false;
    chunk_pool_slot_size_ = slot_size;
    chunk_pool_slot_count_ = count;
    chunk_pool_all_.reserve(count);
    chunk_pool_free_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = (uint8_t*)heap_caps_malloc(slot_size, MALLOC_CAP_SPIRAM);
        if (!p) {
            ESP_LOGW(TAG, "InitChunkPool: allocation failed at %d/%d", i, count);
            break;
        }
        chunk_pool_all_.push_back(p);
        chunk_pool_free_.push_back(p);
    }
    ESP_LOGI(TAG, "InitChunkPool: slots=%d slot_size=%d allocated=%d", count, slot_size, chunk_pool_all_.size());
    return !chunk_pool_all_.empty();
}

void Esp32Music::DestroyChunkPool() {
    std::lock_guard<std::mutex> lk(chunk_pool_mutex_);

    if (chunk_pool_all_.empty()) {
        chunk_pool_slot_size_ = 0;
        chunk_pool_slot_count_ = 0;
        ESP_LOGI(TAG, "DestroyChunkPool: pool already empty");
        return;
    }

    ESP_LOGI(TAG, "DestroyChunkPool: freeing %d slots", chunk_pool_all_.size());

    // 以引用方式释放并把条目标记为 nullptr，避免二次 free
    for (auto &p : chunk_pool_all_) {
        if (p) {
            ESP_LOGD(TAG, "DestroyChunkPool: free %p", p);
            heap_caps_free(p);
            p = nullptr;
        }
    }

    chunk_pool_all_.clear();
    chunk_pool_free_.clear();
    chunk_pool_slot_size_ = 0;
    chunk_pool_slot_count_ = 0;
    ESP_LOGI(TAG, "DestroyChunkPool: freed pool");
}

uint8_t* Esp32Music::AllocChunkFromPool(size_t need_size) {
    // 优先从池里取一个足够大的slot，否则回退到直接分配
    if (need_size == 0) return nullptr;
    std::lock_guard<std::mutex> lk(chunk_pool_mutex_);
    if (!chunk_pool_free_.empty() && need_size <= chunk_pool_slot_size_) {
        uint8_t* p = chunk_pool_free_.back();
        chunk_pool_free_.pop_back();
        // ESP_LOGW(TAG, "AllocChunkFromPool: allocated from pool, need_size=%d", need_size);
        return p;
    }
    // 池空或请求更大，回退到直接分配 SPIRAM
    ESP_LOGE(TAG, "AllocChunkFromPool: 内存池不足, need_size=%d", need_size);
    return (uint8_t*)heap_caps_malloc(need_size, MALLOC_CAP_SPIRAM);
}

void Esp32Music::ReturnChunkToPool(uint8_t* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(chunk_pool_mutex_);
    // 若 p 属于池则放回；否则 free
    auto it = std::find(chunk_pool_all_.begin(), chunk_pool_all_.end(), p);
    if (it != chunk_pool_all_.end()) {
        chunk_pool_free_.push_back(p);
    } else {
        heap_caps_free(p);
    }
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();
        
        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }
            
            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!download_thread_.joinable()) {
                thread_finished = true;
            }
            
            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0) {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }
        
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }
    
    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();
        
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }
            
            // 再次设置停止标志
            is_playing_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!play_thread_.joinable()) {
                thread_finished = true;
            }
        }
        
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }
    
    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();
        // 释放 chunk 池（如果还在）
    DestroyChunkPool();
    ESP_LOGI(TAG, "Music player destroyed successfully");
}


// 停止流式播放
bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    // 先让音频编解码器输出静音
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        // 停止输出
        codec->EnableOutput(false);
        
        // 重置采样率
        ResetSampleRate();

        // 重新启用输出（但此时没有数据，所以是静音）
        codec->EnableOutput(true);
    }
    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }
    
    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;
    is_paused_ = false;
    
    // 清空歌名显示
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        ESP_LOGI(TAG, "Cleared song name display");
    }
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待线程结束（避免重复代码，让StopStreaming也能等待线程完全停止）
    if (download_thread_.joinable()) {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread joined in StopStreaming");
    }
    
    // 等待播放线程结束，使用更安全的方式
    if (play_thread_.joinable()) {
        // 先设置停止标志
        is_playing_ = false;
        
        // 通知条件变量，确保线程能够退出
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        // 使用超时机制等待线程结束，避免死锁
        bool thread_finished = false;
        int wait_count = 0;
        const int max_wait = 30; // 最多等待300毫秒
        
        while (!thread_finished && wait_count < max_wait) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
            
            // 检查线程是否仍然可join
            if (!play_thread_.joinable()) {
                thread_finished = true;
                break;
            }
        }
        
        if (play_thread_.joinable()) {
            if (wait_count >= max_wait) {
                ESP_LOGW(TAG, "Play thread join timeout, detaching thread");
                play_thread_.detach();
            } else {
                play_thread_.join();
                ESP_LOGI(TAG, "Play thread joined in StopStreaming");
            }
        }
    }
        // 清空缓冲区
    ClearAudioBuffer();
    // 销毁 chunk 池（释放预分配内存）
    DestroyChunkPool();

    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}


void Esp32Music::SetMusicEventNextPlay(void)
{
    ManualNextPlay_ = true;
    StopStreaming();
    vTaskDelay(pdMS_TO_TICKS(1000));
    xEventGroupSetBits(event_group_, PLAY_EVENT_NEXT);
}



// 添加一个简单的帧头验证函数
bool Esp32Music::IsValidMp3FrameHeader(uint8_t* header) {
    if (!header) return false;
    
    // 同步字检查 (0xFFF?)
    if (header[0] != 0xFF || (header[1] & 0xE0) != 0xE0) {
        return false;
    }
    
    // 检查MPEG版本 (不能是保留值01)
    uint8_t version = (header[1] >> 3) & 0x03;
    if (version == 0x01) return false;
    
    // 检查层 (不能是保留值00)
    uint8_t layer = (header[1] >> 1) & 0x03;
    if (layer == 0x00) return false;
    
    // 检查比特率索引 (不能是0000或1111)
    uint8_t bitrate_index = (header[2] >> 4) & 0x0F;
    if (bitrate_index == 0x00 || bitrate_index == 0x0F) return false;
    
    // 检查采样率索引 (不能是11)
    uint8_t samplerate_index = (header[2] >> 2) & 0x03;
    if (samplerate_index == 0x03) return false;
    
    return true;
}

// 修改寻找同步字的逻辑
int Esp32Music::FindValidMp3SyncWord(uint8_t* data, int data_len) {
    if (!data || data_len < 4) return -1;
    
    // 先尝试原生的 MP3FindSyncWord
    int sync_offset = MP3FindSyncWord(data, data_len);
    
    // 如果找到了，验证帧头
    if (sync_offset >= 0 && (sync_offset + 4) <= data_len) {
        if (IsValidMp3FrameHeader(&data[sync_offset])) {
            return sync_offset;
        }
        
        // 帧头无效，尝试在附近寻找真正的同步字
        ESP_LOGW(TAG, "Found sync but header invalid, searching nearby...");
    }
    
    // 如果没有找到或者无效，自己搜索
    for (int i = 0; i <= data_len - 4; i++) {
        // 快速检查
        if (data[i] == 0xFF && (data[i+1] & 0xE0) == 0xE0) {
            // 详细验证
            if (IsValidMp3FrameHeader(&data[i])) {
                ESP_LOGI(TAG, "Found valid sync at offset %d (previous was invalid)", i);
                return i;
            }
        }
    }
    
    return -1;
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    total_frames_decoded_ = 0;
    ManualNextPlay_ = false;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        if(!codec){
            ESP_LOGE(TAG, "Audio codec instance is null");
        } else{
            ESP_LOGE(TAG, "Audio codec output not enabled");
            codec->EnableOutput(true);
            ESP_LOGI(TAG, "Current codec output enabled state: %d", codec->output_enabled());
        }
        is_playing_ = false;
        return;
    }
    
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        return;
    }

    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); 
        });
    }
    
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // 分配MP3输入缓冲区
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }
    auto& app = Application::GetInstance();
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;
    static DeviceState current_state = app.GetDeviceState();
    is_paused_ = false;
    is_first_play_ = true;
    using namespace std::chrono;
    steady_clock::time_point listening_start = steady_clock::time_point::min();
    while (is_playing_) {
        // 检查设备状态，只有在空闲状态才播放音乐
        DeviceState previous_state = current_state;
        current_state = app.GetDeviceState();
        if(is_first_play_)
        {
            // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
            if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
                if (current_state == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "Device is in speaking state, switching to listening state for music playback");
                }
                if (current_state == kDeviceStateListening) {
                    ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
                }
                // 切换状态
                app.ToggleChatState(); // 变成待机状态
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            } else if (current_state != kDeviceStateIdle) { // 不是待机状态，就一直卡在这里，不让播放音乐
                ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
                // 如果不是空闲状态，暂停播放
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            
            is_first_play_ = false;
        }
        {
            std::unique_lock<std::mutex> lk(buffer_mutex_);
            if (is_paused_) {
                ESP_LOGI(TAG, "Playback paused, entering timed wait (2s)");
                // 带超时循环等待，周期性检查聆听超时（10s）以自动恢复
                while (is_paused_) {
                    if (!is_playing_) {
                        ESP_LOGI(TAG, "Playback stopping while paused");
                        break;
                    }

                    // 检查当前设备状态并维护聆听计时器
                    current_state = app.GetDeviceState();

                    // 优化：如果设备变为 IDLE，立即恢复播放（无需等待超时）
                    if (current_state == kDeviceStateIdle) {
                        ESP_LOGI(TAG, "Device state is IDLE, auto-resuming playback immediately");
                        // 先解锁再调用 ResumePlayback()（ResumePlayback 内部会加锁并 notify）
                        lk.unlock();
                        ResumePlayback();
                        lk.lock();
                        listening_start = steady_clock::time_point::min();
                        break;
                    }

                    // 如果处于“聆听”状态，启动计时；若处于“说话”状态也重置计时（不计时）
                    if (current_state == kDeviceStateListening) {
                        if (listening_start == steady_clock::time_point::min()) {
                            listening_start = steady_clock::now();
                        } else {
                            auto dur = duration_cast<seconds>(steady_clock::now() - listening_start).count();
                            if (dur >= 10) {
                                ESP_LOGI(TAG, "Listening timeout %llds exceeded, auto-resuming playback", (long long)dur);
                                lk.unlock();
                                ResumePlayback();
                                lk.lock();
                                listening_start = steady_clock::time_point::min();
                                break;
                            }
                        }
                    } else if (current_state == kDeviceStateSpeaking) {
                        // 处于说话状态时重置计时，不触发自动恢复
                        listening_start = steady_clock::time_point::min();
                        ESP_LOGD(TAG, "Device speaking: reset listening timer");
                    } else {
                        // 非聆听/说话状态时重置计时器
                        listening_start = steady_clock::time_point::min();
                    }

                    // 周期性唤醒检查，避免永久阻塞
                    auto status = buffer_cv_.wait_for(lk, std::chrono::seconds(2));
                    if (status == std::cv_status::timeout) {
                        ESP_LOGI(TAG, "Still paused after 2s");
                    } else {
                        ESP_LOGI(TAG, "Woken from pause wait");
                    }
                }
                if (!is_playing_) {
                    ESP_LOGI(TAG, "Playback stopping while paused");
                    break;
                }
                ESP_LOGI(TAG, "Playback resumed after pause");
                // codec/采样率恢复由 ResumePlayback() 负责
            }
        }

        if (previous_state == kDeviceStateIdle && current_state != kDeviceStateIdle) {
            ESP_LOGI(TAG, "Device state changed from IDLE to %d, pausing playback", current_state);
            PausePlayback();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            if (current_state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "Device is in speaking state, switching to listening state for music playback");
            }
            if (current_state == kDeviceStateListening) {
                ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            }
            // 切换状态
            app.ToggleChatState(); // 变成待机状态
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 设备状态检查通过，显示当前播放的歌名
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                static int flag = 1;
                if(flag)
                {
                    if((MusicOrStory_ == MUSIC) && (!current_song_name_.empty()))
                    {    
                        // 格式化歌名显示为《歌名》播放中...
                        std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                        display->SetMusicInfo(formatted_song_name.c_str());
                        ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                    }
                    else if((MusicOrStory_ == STORY) && (!current_story_name_.empty()))
                    {
                        // 格式化故事名显示为《故事名》播放中...
                        std::string formatted_story_name = "《" + current_story_name_ + "》播放中...";
                        display->SetMusicInfo(formatted_story_name.c_str());
                        ESP_LOGI(TAG, "Displaying story name: %s", formatted_story_name.c_str());
                    }
                    flag = 0;
                }

            }
        
        
        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码
            AudioChunk chunk;
            
            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                
                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }
            
            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0) {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // 释放chunk内存
                // heap_caps_free(chunk.data);
                ReturnChunkToPool(chunk.data);
            }
        }
        
        int sync_offset = FindValidMp3SyncWord(read_ptr, bytes_left);
        static int resume_fail_count = 0;
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No valid MP3 sync word found in %d bytes", bytes_left);
            
            // 在断点恢复模式下，更积极地跳过数据
            
            
            if (start_play_offset_ > 0) { // 断点恢复模式
                resume_fail_count++;
                
                if (resume_fail_count > 5) {
                    ESP_LOGW(TAG, "多次找不到有效同步字，放弃断点恢复");
                    // 从头开始播放的逻辑
                    break;
                }
                
                // 跳过更多数据
                int skip = std::min(2048, bytes_left);
                if (skip == 0 && bytes_left == 0) {
                    // 没有数据，等待
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                
                read_ptr += skip;
                bytes_left -= skip;
                ESP_LOGW(TAG, "断点恢复：跳过 %d 字节寻找有效同步字", skip);
            } else {
                // 正常播放模式
                bytes_left = 0;
            }
            continue;
        }

        // 重置失败计数
        resume_fail_count = 0;
        
        // 跳过到同步位置
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // 解码MP3帧
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
        if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 修复：正确的帧时长计算
            // outputSamps是总样本数（声道数 × 每声道样本数）
            int samples_per_channel = mp3_frame_info_.outputSamps / mp3_frame_info_.nChans;
            int frame_duration_ms = (samples_per_channel * 1000) / mp3_frame_info_.samprate;
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;

            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
                    
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        // 使用int32_t避免溢出
                        int32_t mixed = static_cast<int32_t>(left) + static_cast<int32_t>(right);
                        mixed /= 2;
                        mono_buffer[i] = static_cast<int16_t>(mixed);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                            mp3_frame_info_.nChans);
                    // 对于不支持的声道数，可以按单声道处理或跳过
                }
                
                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = frame_duration_ms;  // 使用实际计算的帧时长
                packet.timestamp = current_play_time_ms_;   // 使用累计时间作为时间戳
                
                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);
                
                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // 发送到Application的音频解码队列
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
                
                // 改进的进度打印：避免整数溢出问题
                static int last_reported = 0;
                if (total_played - last_reported >= (128 * 1024)) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                    last_reported = total_played;
                }
            }
            
        } else {
            /* 解码失败：打印原因并找下一个同步字 */
            switch (decode_result) {
            case ERR_MP3_INDATA_UNDERFLOW:        
                ESP_LOGW(TAG, "MP3Decode: -1 (输入数据不足)");
                break;
            case ERR_MP3_MAINDATA_UNDERFLOW:     
                ESP_LOGW(TAG, "MP3Decode: -2 (主数据不足)");
                break;
            case ERR_MP3_FREE_BITRATE_SYNC:       
                ESP_LOGW(TAG, "MP3Decode: -3 (自由码率同步失败)");
                break;
            case ERR_MP3_OUT_OF_MEMORY:           
                ESP_LOGE(TAG, "MP3Decode: -4 (内存不足)");
                break;
            case ERR_MP3_NULL_POINTER:          
                ESP_LOGE(TAG, "MP3Decode: -5 (空指针)");
                break;
            case ERR_MP3_INVALID_FRAMEHEADER:    
                ESP_LOGW(TAG, "MP3Decode: -6 (帧头非法)");
                break;
            case ERR_MP3_INVALID_SIDEINFO:        
                ESP_LOGW(TAG, "MP3Decode: -7 (边信息非法)");
                break;
            case ERR_MP3_INVALID_SCALEFACT:      
                ESP_LOGW(TAG, "MP3Decode: -8 (比例因子非法)");
                break;
            case ERR_MP3_INVALID_HUFFCODES:     
                ESP_LOGW(TAG, "MP3Decode: -9 (Huffman 码表非法)");
                break;
            case ERR_MP3_INVALID_DEQUANTIZE:     
                ESP_LOGW(TAG, "MP3Decode: -10 (反量化错误)");
                break;
            case ERR_MP3_INVALID_IMDCT:          
                ESP_LOGW(TAG, "MP3Decode: -11 (IMDCT 错误)");
                break;
            case ERR_MP3_INVALID_SUBBAND:        
                ESP_LOGW(TAG, "MP3Decode: -12 (子带合成错误)");
                break;
            case ERR_UNKNOWN:                     
            default:
                ESP_LOGE(TAG, "MP3Decode: %d (未知错误)", decode_result);
            }

            // 正常情况：寻找下一个同步字
            int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
            if (sync_offset > 0) {
                read_ptr += sync_offset;
                bytes_left -= sync_offset;
                ESP_LOGW(TAG, "Skip to next sync: offset=%d, left=%d", sync_offset, bytes_left);
            } else if (sync_offset == 0) {
                // 找到了同步字但在当前位置，可能是假同步
                // 跳过至少1字节避免死循环
                read_ptr += 1;
                bytes_left -= 1;
                ESP_LOGW(TAG, "False sync at current position, skipping 1 byte");
            } else {
                // 没有找到同步字
                ESP_LOGW(TAG, "No sync word found in %d remaining bytes", bytes_left);
                bytes_left = 0;
            }
        }
    }
    
    // 清理
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
    }
    
    // 播放结束时进行基本清理，但不调用StopStreaming避免线程自我等待
    //StopStreaming 会在内部 join 播放线程也即是本线程，若从播放线程内调用就会导致自我等待/死锁或未定义行为
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    ESP_LOGI(TAG, "Performing basic cleanup from play thread");
    // 停止播放标志
    is_playing_ = false;

    // 保存断点（按类型）
    if (MusicOrStory_ == MUSIC) {
        SavePlaybackPosition();
    } else {
        SaveStoryPlaybackPosition();
    }

    auto state = app.GetDeviceState();
    if(state == kDeviceStateIdle && !ManualNextPlay_){
        ESP_LOGI(TAG, "Device is idle, preparing to play next track");
        xEventGroupSetBits(event_group_, PLAY_EVENT_NEXT);
        // if(IfNodeIsEnd())
        // {
        //     if(MusicPlayback_mode_ == PLAYBACK_MODE_ONCE)
        //     {
        //         ESP_LOGI(TAG, "Once playback mode active, not sending further commands");
        //         return;
        //     }
        //     else if (MusicPlayback_mode_ == PLAYBACK_MODE_ORDER)
        //     {
        //         NextPlayIndexOrder(current_playlist_name_);

        //     }
        //     else if (MusicPlayback_mode_ == PLAYBACK_MODE_RANDOM)
        //     {
        //         NextPlayIndexRandom(current_playlist_name_);

        //     }
        //     EnableRecord(true);
        //     PlayPlaylist(current_playlist_name_);
        // }
        // else
        // {
        //     EnableRecord(false);
        //     SetPlayIndex(default_musiclist_, NextNodeIndex());
        //     PlayPlaylist(default_musiclist_);
        // }
    }

    
    // auto state = app.GetDeviceState();
    // if(state == kDeviceStateIdle){
        
    //     if(MusicPlayback_mode_ == PLAYBACK_MODE_ONCE)
    //         {
    //             ESP_LOGI(TAG, "Once playback mode active, stopping playback");
    //             //等待线程自动结束，不用做任何处理
    //             std::string msg = "再见,你不需要回应"; 
    //             app.SendMessage(msg);
    //         }
    //     else{
    //             std::string msg = "播放下一首";
    //             app.SendMessage(msg);
    //         }        
    // }
    
}


void Esp32Music::NextPlayTask(void* arg) {
    
    while(1)
    {
        EventBits_t bits = xEventGroupWaitBits(event_group_, PLAY_EVENT_NEXT, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & PLAY_EVENT_NEXT) {
            if(IfNodeIsEnd())
            {
                if(MusicPlayback_mode_ == PLAYBACK_MODE_ONCE)
                {
                    ESP_LOGI(TAG, "Once playback mode active, not sending further commands");
                    return;
                }
                else if (MusicPlayback_mode_ == PLAYBACK_MODE_ORDER)
                {
                    NextPlayIndexOrder(current_playlist_name_);

                }
                else if (MusicPlayback_mode_ == PLAYBACK_MODE_RANDOM)
                {
                    NextPlayIndexRandom(current_playlist_name_);

                }
                StopStreaming();
                EnableRecord(true);
                PlayPlaylist(current_playlist_name_);
            }
            else
            {
                StopStreaming();
                EnableRecord(false);
                SetPlayIndex(default_musiclist_, NextNodeIndex());
                PlayPlaylist(default_musiclist_);
            }
        }
    }
    
}
// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            // heap_caps_free(chunk.data);
            ReturnChunkToPool(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    CleanupMp3Decoder();
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 && 
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
                codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {  // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}


// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 * 
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数




// ========== SD卡相关函数 ==========

/**
 * @brief 检查文件是否存在
 */
static bool file_exists(const std::string& filename) {
    struct stat st;
    return (stat(filename.c_str(), &st) == 0);
}

/**
 * @brief 获取文件大小
 */
static size_t get_file_size(const std::string& filename) {
    struct stat st;
    if (stat(filename.c_str(), &st) == 0) {
        return st.st_size;
    }
    return 0;
}

/**
 * @brief 获取文件扩展名
 */
static std::string get_file_extension(const std::string& filename) {
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos != std::string::npos) {
        return filename.substr(dot_pos + 1);
    }
    return "";
}

/**
 * @brief 检查是否是目录
 */
static bool is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return false;
}



// ========== 新增的SD卡播放功能 ==========


void Esp32Music::SetLoopMode(bool loop) {
    if(MusicOrStory_ == STORY)
        StoryPlayback_mode_ = loop ? PLAYBACK_MODE_LOOP : PLAYBACK_MODE_ONCE;
    else
    MusicPlayback_mode_ = loop ? PLAYBACK_MODE_LOOP : PLAYBACK_MODE_ONCE;
}
void Esp32Music::SetRandomMode(bool random) {
    if(MusicOrStory_ == STORY)
        StoryPlayback_mode_ = random ? PLAYBACK_MODE_RANDOM : PLAYBACK_MODE_ONCE;
    else
    MusicPlayback_mode_ = random ? PLAYBACK_MODE_RANDOM : PLAYBACK_MODE_ONCE;
}
void Esp32Music::SetOnceMode(bool once) {
    if(MusicOrStory_ == STORY)
        StoryPlayback_mode_ = once ? PLAYBACK_MODE_ONCE : PLAYBACK_MODE_ONCE;
    else
    MusicPlayback_mode_ = once ? PLAYBACK_MODE_ONCE : PLAYBACK_MODE_ONCE;
}
void Esp32Music::SetOrderMode(bool order) {
    if(MusicOrStory_ == STORY)
        StoryPlayback_mode_ = order ? PLAYBACK_MODE_ORDER : PLAYBACK_MODE_ONCE;
    else
    MusicPlayback_mode_ = order ? PLAYBACK_MODE_ORDER : PLAYBACK_MODE_ONCE;
}

PlaybackMode Esp32Music::GetPlaybackMode() {
    if(MusicOrStory_ == STORY)
        return StoryPlayback_mode_;
    else
        return MusicPlayback_mode_;
}

/**
 * @brief 从SD卡播放音乐文件
 * @param file_path SD卡上的音乐文件路径
 * @param song_name 歌曲名称（用于显示）
 * @return 是否成功开始播放
 */
bool Esp32Music::PlayFromSD(const std::string& file_path, const std::string& song_name) {
    ESP_LOGI(TAG, "Starting to play music from SD card: %s", file_path.c_str());
    
    // 检查文件是否存在
    if (!file_exists(file_path)) {
        ESP_LOGE(TAG, "File does not exist: %s", file_path.c_str());
        return false;
    }
    
    // 获取文件大小
    size_t file_size = get_file_size(file_path);
    if (file_size == 0) {
        ESP_LOGE(TAG, "File is empty: %s", file_path.c_str());
        return false;
    }
    
        // 检查文件格式
    std::string extension = get_file_extension(file_path);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    if (extension != "mp3" && extension != "MP3") {
        ESP_LOGW(TAG, "File format may not be supported: %s", extension.c_str());
        // 继续尝试播放，让解码器处理
    }

    ESP_LOGI(TAG, "SD card file size: %d bytes", file_size);
    


    // 保存歌名用于显示
    if(MusicOrStory_ == MUSIC)
    {
        if(!song_name.empty())
            current_song_name_ = song_name;
        else
        {
            size_t pos = file_path.find_last_of("/\\");
            if (pos != std::string::npos) 
                current_song_name_ = file_path.substr(pos + 1);
            
            pos = current_song_name_.find_last_of('.');
            if (pos != std::string::npos)
                current_song_name_ = current_song_name_.substr(0, pos);
        }
        auto pos = current_song_name_.find_first_of('-');
        if(SaveMusicRecord_ == true)
        {
            ESP_LOGI(TAG, "Updating music record list for song: %s", current_song_name_.c_str());
            if(pos != std::string::npos)
            {
                UpdateMusicRecordList(current_song_name_.substr(0, pos),current_song_name_.substr(pos + 1));
            }
            else
                UpdateMusicRecordList("",current_song_name_);

            SaveMusicRecord_ = false;
        }


    }
    else
    {
        if(!song_name.empty())
            current_story_name_ = song_name;
        else
        {
            size_t pos = file_path.find_last_of("/\\");
            current_story_name_ = file_path.substr(pos + 1);
        }
    }

    
    // 停止之前的播放
    StopStreaming();
    
    return StartSDCardStreaming(file_path);
}


void Esp32Music::ResumePlayback() {
    std::lock_guard<std::mutex> lk(buffer_mutex_);
    if (!is_paused_) return;
    is_paused_ = false;
    // 确保 codec 输出及采样率恢复
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->EnableOutput(true);
        ResetSampleRate();
    }
    buffer_cv_.notify_all();
    ESP_LOGI(TAG, "ResumePlayback: resumed and notified");
}

void Esp32Music::PausePlayback() {
    std::lock_guard<std::mutex> lk(buffer_mutex_);
    if (is_paused_) return;
    is_paused_ = true;
    ESP_LOGI(TAG, "PausePlayback: paused");
}

/**
 * @brief 开始SD卡流式播放
 * @param file_path SD卡文件路径
 * @return 是否成功
 */
bool Esp32Music::StartSDCardStreaming(const std::string& file_path) {
    if (file_path.empty()) {
        ESP_LOGE(TAG, "File path is empty");
        return false;
    }
    
    InitializeMp3Decoder();

    ESP_LOGD(TAG, "Starting SD card streaming for: %s", file_path.c_str());
    
    // 停止之前的播放
    is_downloading_ = false;
    is_playing_ = false;
    
    // 等待之前的线程完全结束
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        play_thread_.join();
    }

    // 初始化 chunk 池，减少 heap 分配碎片（100 个 5KB 槽）
    InitChunkPool(100, 5120);
    
    // 配置线程栈大小
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192+1024;
    cfg.prio = 5;
    cfg.thread_name = "sd_card_stream";
    esp_pthread_set_cfg(&cfg);
    
    // 开始SD卡读取线程
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::ReadFromSDCard, this, file_path);
    
    // 开始播放线程
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "SD card streaming threads started successfully");
    return true;
}

// 修改 ReadFromSDCard：支持 start_play_offset_、维护 current_play_file_offset_
void Esp32Music::ReadFromSDCard(const std::string& file_path) {

    ESP_LOGD(TAG, "Starting audio stream reading from SD card: %s", file_path.c_str());
    
    FILE* file = fopen(file_path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path.c_str());
        is_downloading_ = false;
        return;
    }

    // 在打开后，如果有请求的 start_play_offset_ 则 seek 到该位置
    // 断点恢复处理
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        if (start_play_offset_ > 0) {
            // 从保存位置回退2KB（为了确保从帧边界开始）
            size_t safe_offset = (start_play_offset_ > 2048) ? 
                                start_play_offset_ - 2048 : 0;
            
            ESP_LOGI(TAG, "断点恢复：从 %d 回退到 %d", start_play_offset_, safe_offset);
            
            if (fseek(file, safe_offset, SEEK_SET) == 0) {
                current_play_file_offset_ = safe_offset;
            } else {
                ESP_LOGW(TAG, "回退失败，从头开始");
                current_play_file_offset_ = 0;
            }
            start_play_offset_ = 0;
        } else {
            current_play_file_offset_ = 0;
        }
        current_play_file_ = file;
    }
    
    ESP_LOGI(TAG, "Started reading audio stream from SD card");
    
    // 分块读取音频数据
    const size_t chunk_size = 4096;
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        fclose(file);
        {
            std::lock_guard<std::mutex> lock(current_play_file_mutex_);
            current_play_file_ = nullptr;
            current_play_file_offset_ = 0;
        }
        is_downloading_ = false;
        return;
    }
    
    size_t total_read = 0;
    bool first_resume = true;
    auto& app = Application::GetInstance();

    while (is_downloading_ && is_playing_) {
        
        // 在真正做文件 I/O 前，如果处于暂停则等待（避免 pause 时继续 fread/分配导致缓冲溢满）
        {
            std::unique_lock<std::mutex> lk(buffer_mutex_);
            if (is_paused_) {
                ESP_LOGI(TAG, "Read thread paused, waiting for resume");
                buffer_cv_.wait(lk, [this]{ return !is_paused_ || !is_downloading_ || !is_playing_; });
                if (!is_downloading_ || !is_playing_) break;
                ESP_LOGI(TAG, "Read thread resumed");
            }
        }
        
        // 保护 file I/O，避免 concurrent fclose 导致 crash
        size_t bytes_read = 0;
        {
            std::lock_guard<std::mutex> f_lock(current_play_file_mutex_);
            if (!file) break;
            bytes_read = fread(buffer, 1, chunk_size, file);
        }
        if (bytes_read == 0) {
            if (feof(file)) {
                // 如果当前文件偏移已经达到文件大小，则从头开始播放（回绕）
                size_t file_size = get_file_size(file_path);
                size_t cur_offset = 0;
                {
                    std::lock_guard<std::mutex> lock(current_play_file_mutex_);
                    cur_offset = current_play_file_offset_;
                }
                if (file_size > 0 && cur_offset >= file_size && first_resume) {
                    ESP_LOGI(TAG, "SD card file reached EOF at offset %zu (size %zu), rewinding to 0", cur_offset, file_size);
                    if (fseek(file, 0, SEEK_SET) == 0) {
                        {
                            std::lock_guard<std::mutex> lock(current_play_file_mutex_);
                            current_play_file_offset_ = 0;
                            current_play_file_ = file;
                        }
                        total_read = 0;
                        // 继续循环从文件头读取
                        continue;
                    } else {
                        ESP_LOGW(TAG, "Failed to rewind SD file, closing read loop");
                    }
                } else {
                    ESP_LOGI(TAG, "SD card file read completed, total: %u bytes", (unsigned)total_read);
                }
            } else {
                ESP_LOGE(TAG, "Failed to read from file");
            }
            break;
        }
        first_resume = false;
        // 更新当前播放文件偏移（先更新）
        {
            std::lock_guard<std::mutex> lock(current_play_file_mutex_);
            current_play_file_offset_ += bytes_read;
        }

        uint8_t* chunk_data = AllocChunkFromPool(bytes_read);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk (pool and fallback)");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);
       

        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);

            buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_read += bytes_read;

                // 通知播放线程有新数据
                buffer_cv_.notify_one();
                if (total_read % (256 * 1024) == 0) {
                    ESP_LOGI(TAG, "Read %d bytes from SD, buffer size: %d", total_read, buffer_size_);
                }
            } else {
                ReturnChunkToPool(chunk_data);
                break;
            }
        }
    }
    
    heap_caps_free(buffer);
    fclose(file);

    // 清理 current_play_file_（保留偏移读取，对外可查询）
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        current_play_file_ = nullptr;
    }

    is_downloading_ = false;
    
    // 通知播放线程读取完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "SD card read thread finished");
}



// ========== SD卡音乐库扫描功能 ==========

char* Esp32Music::ps_strdup(const std::string &s) {
    size_t len = s.size() + 1;
    char *p = (char*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!p) return nullptr;
    memcpy(p, s.c_str(), len);
    return p;
}
void Esp32Music::ps_free_str(char *p) {
    if (p) heap_caps_free(p);
}

// 释放 PSRAM 中的音乐库（调用时需持有 music_library_mutex_）
void Esp32Music::free_ps_music_library_locked() {
    if (!ps_music_library_) return;
    for (size_t i = 0; i < ps_music_count_; ++i) {
        auto &e = ps_music_library_[i];
        ps_free_str(e.file_path); e.file_path = nullptr;
        ps_free_str(e.file_name); e.file_name = nullptr;
        ps_free_str(e.song_name); e.song_name = nullptr;
        ps_free_str(e.artist); e.artist = nullptr;
        ps_free_str(e.artist_norm); e.artist_norm = nullptr;
        ps_free_str(e.token_norm); e.token_norm = nullptr;
    }
    heap_caps_free(ps_music_library_);
    ps_music_library_ = nullptr;
    ps_music_count_ = 0;
    ps_music_capacity_ = 0;
}

// 在 PSRAM 数组中追加一条（调用时需持有 music_library_mutex_）
// 返回 true 表示追加成功
bool Esp32Music::ps_add_music_info_locked(const MusicFileInfo &info) {
    // 1.5x 或最少初始容量 64
    size_t need = ps_music_count_ + 1;
    if (need > ps_music_capacity_) {
        size_t new_cap = ps_music_capacity_ ? (ps_music_capacity_ * 3) / 2 : 64;
        if (new_cap < need) new_cap = need;
        // 在 PSRAM 中分配新数组
        PSMusicInfo *new_arr = (PSMusicInfo*)heap_caps_malloc(new_cap * sizeof(PSMusicInfo), MALLOC_CAP_SPIRAM);
        if (!new_arr) return false;
        // 初始化新内存
        memset(new_arr, 0, new_cap * sizeof(PSMusicInfo));
        // 复制旧条目结构（指针值拷贝）
        if (ps_music_library_ && ps_music_count_ > 0) {
            memcpy(new_arr, ps_music_library_, ps_music_count_ * sizeof(PSMusicInfo));
            heap_caps_free(ps_music_library_);
        }
        ps_music_library_ = new_arr;
        ps_music_capacity_ = new_cap;
    }

    // 填充新条目（逐字段在 PSRAM 分配字符串副本）
    PSMusicInfo &dst = ps_music_library_[ps_music_count_];
    dst.file_path = ps_strdup(info.file_path);
    dst.file_name = ps_strdup(info.file_name);
    dst.song_name = ps_strdup(info.song_name);
    dst.artist = ps_strdup(info.artist);
    dst.artist_norm = ps_strdup(info.artist_norm);

    // 生成 token-normalized 字符串并存到 PSRAM（避免查询时重复分配）
    std::string token = NormalizeForToken(info.file_name);
    dst.token_norm = ps_strdup(token);

    dst.file_size = info.file_size;
    dst.duration = info.duration;

    // 检查是否有字符串分配失败，若失败则释放刚分配字段并返回 false（保留已存在条目）
    if ((!dst.file_path) || (!dst.file_name) || (!dst.song_name) || (!dst.artist) || (!dst.artist_norm) || (!dst.token_norm)) {
        ps_free_str(dst.file_path); dst.file_path = nullptr;
        ps_free_str(dst.file_name); dst.file_name = nullptr;
        ps_free_str(dst.song_name); dst.song_name = nullptr;
        ps_free_str(dst.artist); dst.artist = nullptr;
        ps_free_str(dst.artist_norm); dst.artist_norm = nullptr;
        ps_free_str(dst.token_norm); dst.token_norm = nullptr;
        return false;
    }

    ps_music_count_++;
    return true;
}

void Esp32Music::ScanDirectoryRecursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path.c_str());
        return;
    }
    struct dirent* entry;
    int file_count = 0;
    int dir_count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        std::string full_path = path + "/" + entry->d_name;
        if (entry->d_type == DT_DIR) {
            // ESP_LOGD(TAG, "Scanning subdirectory: %s", full_path.c_str());
            ScanDirectoryRecursive(full_path);
            dir_count++;
        } else if (entry->d_type == DT_REG) {
            if (IsMusicFile(full_path)) {
                MusicFileInfo music_info = ExtractMusicInfo(full_path);
                {
                    std::lock_guard<std::mutex> lock(music_library_mutex_);
                    if (!ps_add_music_info_locked(music_info)) {
                        ESP_LOGW(TAG, "Failed to add music info into PSRAM for %s", full_path.c_str());
                        // 失败时继续扫描，其它条目保留
                    }
                }
                file_count++;
                // if (file_count % 10 == 0) ESP_LOGI(TAG, "Scanned %d music files...", file_count);
                // ESP_LOGD(TAG, "Found music file: %s (%d bytes)", music_info.file_name.c_str(), (int)music_info.file_size);
            }
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Scanned directory %s: %d files, %d subdirectories", path.c_str(), file_count, dir_count);
}


bool Esp32Music::ScanMusicLibrary(const std::string& music_folder) {
    ESP_LOGI(TAG, "Scanning music library from: %s", music_folder.c_str());
    if (!file_exists(music_folder) || !is_directory(music_folder)) {
        ESP_LOGE(TAG, "Music folder invalid: %s", music_folder.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        free_ps_music_library_locked();
        music_library_scanned_ = false;
    }
    ScanDirectoryRecursive(music_folder);
    {
        /* 1. 申请两套视图 */
        size_t n = ps_music_count_;
        music_view_        = (MusicView *)heap_caps_malloc(n * sizeof(MusicView), MALLOC_CAP_SPIRAM);
        music_view_art_song_ = (MusicView *)heap_caps_malloc(n * sizeof(MusicView), MALLOC_CAP_SPIRAM);
        music_view_singer_ = (MusicView *)heap_caps_malloc(n * sizeof(MusicView), MALLOC_CAP_SPIRAM);
        
        /* 2. 填指针（零拷贝）*/
        for (size_t i = 0; i < n; ++i) {
            music_view_[i].song_name   = ps_music_library_[i].song_name;
            music_view_[i].artist_norm = ps_music_library_[i].artist_norm;
            music_view_[i].idx         = i;

            music_view_art_song_[i]    = music_view_[i];   // 先复拷一份
            music_view_singer_[i]      = music_view_[i];   // 先复拷一份
        }

        /* 3. 歌名视图排序 */
        auto cmpSong = [](const void *a, const void *b){
            return strcmp(((const MusicView*)a)->song_name,
                          ((const MusicView*)b)->song_name);
        };
        qsort(music_view_, n, sizeof(MusicView), cmpSong);

        /* 4. artist-song 视图排序 */
        auto cmpArtSong = [](const void *a, const void *b){
            const MusicView *v1 = (const MusicView*)a;
            const MusicView *v2 = (const MusicView*)b;
            char k1[256], k2[256];
            snprintf(k1, sizeof(k1), "%s-%s", v1->artist_norm, v1->song_name);
            snprintf(k2, sizeof(k2), "%s-%s", v2->artist_norm, v2->song_name);
            return strcmp(k1, k2);
        };
        qsort(music_view_art_song_, n, sizeof(MusicView), cmpArtSong);



        /* 5. 歌手视图排序 */
        auto cmpSinger = [](const void *a, const void *b){
            return strcmp(((const MusicView*)a)->artist_norm,
                        ((const MusicView*)b)->artist_norm);
        };
        qsort(music_view_singer_, n, sizeof(MusicView), cmpSinger);
        
    }
    ESP_LOGI(TAG, "Music library scan completed, found %u music files", (unsigned)ps_music_count_);
    return ps_music_count_ > 0;
}

void Esp32Music::ScanAndLoadMusic() {
    ESP_LOGI(TAG, "Initializing default playlists from SD card music library");
    {
        // 清理并扫描
        if (!ScanMusicLibrary("/sdcard/音乐")) {
            ESP_LOGW(TAG, "ScanMusicLibrary failed or SD not ready");
        }
    }
    LoadPlaybackPosition();
}

const PSMusicInfo* Esp32Music::GetMusicLibrary(size_t &out_count) const {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    out_count = ps_music_count_;
    return ps_music_library_;
}


MusicFileInfo Esp32Music::GetMusicInfo(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    for (size_t i = 0; i < ps_music_count_; ++i) {
        if (ps_music_library_[i].file_path && file_path == ps_music_library_[i].file_path) {
            MusicFileInfo info;
            info.file_path = std::string(ps_music_library_[i].file_path);
            info.file_name = ps_music_library_[i].file_name ? std::string(ps_music_library_[i].file_name) : std::string();
            info.song_name = ps_music_library_[i].song_name ? std::string(ps_music_library_[i].song_name) : std::string();
            info.artist = ps_music_library_[i].artist ? std::string(ps_music_library_[i].artist) : std::string();
            info.artist_norm = ps_music_library_[i].artist_norm ? std::string(ps_music_library_[i].artist_norm) : std::string();
            info.file_size = ps_music_library_[i].file_size;
            info.duration = ps_music_library_[i].duration;
            return info;
        }
    }
    return MusicFileInfo();
}


bool Esp32Music::IsMusicFile(const std::string& file_path) const {
    std::string extension = get_file_extension(file_path);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    // 支持的音乐文件格式
    const char* music_extensions[] = {
        "mp3", "wav", "flac", "aac", "m4a", "ogg", "wma", nullptr
    };
    
    for (int i = 0; music_extensions[i] != nullptr; i++) {
        if (extension == music_extensions[i]) {
            return true;
        }
    }
    
    return false;
}


MusicFileInfo Esp32Music::ExtractMusicInfo(const std::string& file_path) const {
    MusicFileInfo info;
    info.file_path = file_path;


    size_t last_slash = file_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        info.file_name = file_path.substr(last_slash + 1);
    } else {
        info.file_name = file_path;
    }

    // 使用已有的 ParseSongMeta 提取 artist/title 并规范化
    SongMeta meta = ParseSongMeta(info.file_name);

    // 填充到 MusicFileInfo 中
    info.song_name = meta.norm_title;
    info.artist = meta.artist;
    info.artist_norm = meta.norm_artist;
    // ESP_LOGI(TAG, "Extracted music info - File: %s, Artist: %s, Song: %s", 
    //         info.file_name.c_str(), info.artist.c_str(), info.song_name.c_str());
    // 获取文件大小
    info.file_size = get_file_size(file_path);

    return info;
}


std::string Esp32Music::NormalizeForToken(const std::string &s) const {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (std::isalnum(c) || c == '-') out.push_back(static_cast<char>(std::tolower(c)));
            else if (out.empty() || out.back() != ' ') out.push_back(' ');
        } else {
            size_t seq_len = 1;
            if ((c & 0xE0) == 0xC0) seq_len = 2;
            else if ((c & 0xF0) == 0xE0) seq_len = 3;
            else if ((c & 0xF8) == 0xF0) seq_len = 4;
            if (i + seq_len <= s.size()) {
                out.append(s.substr(i, seq_len));
                i += seq_len - 1;
            }
        }
    }
    // trim
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string NormalizeForSearch(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            // ASCII：保留字母数字与连字符 '-'（字母转小写），丢弃空白与其它标点
            if (std::isalnum(c) || c == '-') {
                out.push_back(static_cast<char>(std::tolower(c)));
            }
        } else {
            // 非 ASCII：保留完整的 UTF-8 多字节序列
            size_t seq_len = 1;
            if ((c & 0xE0) == 0xC0) seq_len = 2;
            else if ((c & 0xF0) == 0xE0) seq_len = 3;
            else if ((c & 0xF8) == 0xF0) seq_len = 4;
            // 安全拷贝，避免越界
            if (i + seq_len <= s.size()) {
                out.append(s.substr(i, seq_len));
                i += seq_len - 1;
            } else {
                // 不完整序列时把剩余都追加并结束
                out.append(s.substr(i));
                break;
            }
        }
    }
    return out;
}

// 歌手和歌名以及它们的规范化形式
SongMeta ParseSongMeta(const std::string& filename) {
    SongMeta meta;
    std::string name = filename;


    size_t pos = name.find_last_of("/\\");
    if (pos != std::string::npos) name = name.substr(pos + 1);

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);

    meta.title = name;
    meta.artist.clear();

    size_t dash = name.find('-');
    if (dash != std::string::npos) {
        std::string left = name.substr(0, dash);
        std::string right = name.substr(dash + 1);

        while (!left.empty() && std::isspace((unsigned char)left.front())) left.erase(left.begin());
        while (!left.empty() && std::isspace((unsigned char)left.back())) left.pop_back();
        while (!right.empty() && std::isspace((unsigned char)right.front())) right.erase(right.begin());
        while (!right.empty() && std::isspace((unsigned char)right.back())) right.pop_back();

        // 若左侧含有字母或非 ASCII 字节（如中文），且右侧非空，则认为是 artist-title
        bool left_has_alpha_or_nonascii = false;
        for (unsigned char ch : left) {
            if (ch >= 0x80) { left_has_alpha_or_nonascii = true; break; } // 非 ASCII，视为有效（中文）
            if (std::isalpha(ch)) { left_has_alpha_or_nonascii = true; break; }
        }
        bool right_nonempty = !right.empty();
        if (left_has_alpha_or_nonascii && right_nonempty) {
            meta.artist = left;
            meta.title = right;
        } else {
            // 否则保留整个字符串为 title
            meta.title = name;
            meta.artist.clear();
        }
    }

    // 剔除常见后缀括注（简洁处理：去掉括号或中括号及其中内容）
    auto strip_bracket_suffix = [](std::string &s) {
        size_t p = s.find_first_of("([");
        if (p != std::string::npos) {
            s.erase(p);
            
            while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        }
    };
    strip_bracket_suffix(meta.title);
    if (!meta.artist.empty()) strip_bracket_suffix(meta.artist);

    // 生成规范化字段（用于不区分空格/大小写/特殊字符的匹配）
    meta.norm_title = NormalizeForSearch(meta.title);
    meta.norm_artist = meta.artist.empty() ? std::string() : NormalizeForSearch(meta.artist);

    return meta;
}







// ========== 播放列表功能实现 ==========

bool Esp32Music::CreatePlaylist(const std::string& playlist_name, const std::vector<std::string>& file_paths) {
    if (playlist_name.empty()) {
        ESP_LOGE(TAG, "Playlist name cannot be empty");
        return false;
    }
    // 检查文件是否存在
    for (const auto& file_path : file_paths) {
        if (!file_exists(file_path)) {
            ESP_LOGW(TAG, "File does not exist: %s", file_path.c_str());
        }
    }
    std::lock_guard<std::mutex> lock(music_library_mutex_);

    playlist_.file_paths.clear();
    playlist_.play_index = 0;
    playlist_.last_play_index = 0;
    // 创建新播放列表
    playlist_ = Playlist(playlist_name);
    playlist_.file_paths = file_paths;

    ESP_LOGI(TAG, "Created playlist '%s' with %d songs", playlist_.name.c_str(), playlist_.file_paths.size());
    return true;
}


void Esp32Music::SetPlayIndex(const std::string& playlist_name, int index) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    if(playlist_name == default_musiclist_)
    {
        last_play_index_ = play_index_;
        play_index_ = index;
        if(play_index_ >= ps_music_count_)
            play_index_ = ps_music_count_;
    }
    else
    {
        playlist_.last_play_index = playlist_.play_index;
        playlist_.play_index = index;
        if(playlist_.play_index >= playlist_.file_paths.size())
            playlist_.play_index = playlist_.file_paths.size();
    }
}

void Esp32Music::NextPlayIndexOrder(std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    if(playlist_name == default_musiclist_)
    {
        last_play_index_ = play_index_;
        play_index_++;
        if(play_index_ >= ps_music_count_)
            play_index_ = 0;
        ESP_LOGI(TAG, "Order next play index: %d", play_index_);
        return;
    }
    else
    {
        playlist_.last_play_index = playlist_.play_index;
        playlist_.play_index++;
        if(playlist_.play_index >= playlist_.file_paths.size())
            playlist_.play_index = 0;
        ESP_LOGI(TAG, "Order next play index: %d", playlist_.play_index);
    }
}


std::string Esp32Music::SearchMusicFromlistByIndex(std::string list) const {
    if(list != default_musiclist_)
        return playlist_.file_paths[playlist_.play_index];

    return ps_music_library_[play_index_].song_name;
}
std::string Esp32Music::GetCurrentPlayList(void) {

    return current_playlist_name_;
}

void Esp32Music::NextPlayIndexRandom(std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    int index;
    if (playlist_name == default_musiclist_) {
        ESP_LOGI(TAG, "Playing default music library");
        do {
            last_play_index_ = play_index_;
            play_index_ = esp_random() % ps_music_count_;
        } while (play_index_ == last_play_index_);
        index = play_index_;
    }
    else
    {
        do {
            playlist_.last_play_index = playlist_.play_index;
            playlist_.play_index = esp_random() % playlist_.file_paths.size();
        } while (playlist_.play_index == playlist_.last_play_index);
        index = playlist_.play_index;
    }
    ESP_LOGI(TAG, "Random next play index: %d", index);
}



void Esp32Music::SetCurrentPlayList(const std::string& playlist_name) {
    current_playlist_name_ = playlist_name;
}

bool Esp32Music::PlayPlaylist(const std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);

    if (playlist_name == default_musiclist_) {
        ESP_LOGI(TAG, "Playing default music library");
        bool result = PlayFromSD(ps_music_library_[play_index_].file_path);
        return result;
    }
    else
    {
        bool result = PlayFromSD(playlist_.file_paths[playlist_.play_index]);
        return result;
    }
}



bool Esp32Music:: IsSubsequence(const char* q, const char* t)const {
    if (!q || !t) return false;
    size_t iq = 0, it = 0;
    const unsigned char *pq = (const unsigned char*)q;
    const unsigned char *pt = (const unsigned char*)t;
    while (pq[iq] && pt[it]) {
        if (pq[iq] == pt[it]) { ++iq; ++it; if (!pq[iq]) return true; }
        else ++it;
    }
    return pq[iq] == 0;
}

std::vector<std::string> Esp32Music:: SplitTokensNoAlloc(const std::string &token_norm)const {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < token_norm.size()) {
        while (i < token_norm.size() && token_norm[i] == ' ') ++i;
        size_t j = i;
        while (j < token_norm.size() && token_norm[j] != ' ') ++j;
        if (j > i) t.emplace_back(token_norm.substr(i, j - i));
        i = j;
    }
    return t;
}

bool Esp32Music::TokenSeqMatchUsingTokenNormNoAlloc(const char* tgt_token_norm, const std::vector<std::string>& qtokens) const{
    if (!tgt_token_norm) return false;
    const char* p = tgt_token_norm;
    for (const auto &qt : qtokens) {
        if (qt.empty()) continue;
        const char* found = strstr(p, qt.c_str());
        if (!found) return false;
        p = found + qt.size();
    }
    return true;
}

void Esp32Music:: ComputeFreqVector(const char* s, std::vector<int>& freq) const{
    std::fill(freq.begin(), freq.end(), 0);
    if (!s) return;
    const unsigned char* ps = (const unsigned char*)s;
    for (size_t k = 0; ps[k]; ++k) ++freq[ps[k]];
}

int Esp32Music::OverlapScoreFromFreq(const std::vector<int>& freq_q, const std::vector<int>& freq_t, int qlen)const {
    int common = 0;
    for (int b = 0; b < 256; ++b) common += std::min(freq_q[b], freq_t[b]);
    double overlap = (qlen > 0) ? (double)common / (double)qlen : 0.0;
    return static_cast<int>(overlap * 100.0);
}

int Esp32Music::SearchMusicIndexFromlist(std::string name) const
{
    if (!music_view_) return -1;

    std::string orig_query = name;
    name = NormalizeForSearch(name);
    if (name.empty()) return -1;

    /* 精确二分 */
    auto cmp = [](const void *k, const void *e){
        return strcmp((const char*)k, ((const MusicView*)e)->song_name);
    };
    void *found = bsearch(name.c_str(), music_view_,
                          ps_music_count_, sizeof(MusicView), cmp);
    if (found) {
        return ((MusicView*)found)->idx;
    }

    // 预处理 token 查询
    std::string q_token_norm = NormalizeForToken(orig_query);
    auto q_tokens = SplitTokensNoAlloc(q_token_norm);
    const char* q_c = name.c_str();

    // 预计算 query 的字节频率（堆上）
    std::vector<int> freq_q(256, 0);
    {
        const unsigned char* pq = (const unsigned char*)q_c;
        for (size_t k = 0; pq[k]; ++k) ++freq_q[pq[k]];
    }

    int best_idx = -1;
    int best_score = INT_MIN;
    int best_len_diff = INT_MAX;
    std::vector<int> freq_t(256);

    std::lock_guard<std::mutex> lock(music_library_mutex_);
    for (size_t i = 0; i < ps_music_count_; ++i) {
        const PSMusicInfo &m = ps_music_library_[i];
        if (!m.song_name) continue;
        const char* tgt = m.song_name;
        if (!tgt) continue;

        int score = 0;

        if (strstr(tgt, q_c) != nullptr) score += 400;
        if (strncmp(tgt, q_c, strlen(q_c)) == 0) score += 120;
        if (IsSubsequence(q_c, tgt)) score += 150;

        const char* targ_token = m.token_norm ? m.token_norm : (m.file_name ? m.file_name : m.song_name);
        if (!q_tokens.empty() && targ_token && TokenSeqMatchUsingTokenNormNoAlloc(targ_token, q_tokens)) score += 200;

        ComputeFreqVector(tgt, freq_t);
        int qlen = (int)strlen(q_c);
        int tgtlen = (int)strlen(tgt);
        score += OverlapScoreFromFreq(freq_q, freq_t, qlen);

        int len_diff = std::abs(qlen - tgtlen);
        score += std::max(0, 30 - len_diff);

        bool replace = false;
        if (score > best_score) replace = true;
        else if (score == best_score && len_diff < best_len_diff) replace = true;

        if (replace) {
            best_score = score;
            best_idx = static_cast<int>(i);
            best_len_diff = len_diff;
            if (best_score >= 700) break;
        }
    }

    if (best_idx < 0) {
        ESP_LOGW(TAG, "no fuzzy match for: %s", orig_query.c_str());
        return -1;
    }

    ESP_LOGI(TAG, "fuzzy best=%s idx=%d score=%d",
             ps_music_library_[best_idx].song_name ? ps_music_library_[best_idx].song_name : "<nil>",
             best_idx, best_score);
    return best_idx;
}

int Esp32Music::SearchMusicIndexFromlistByArtSong(std::string songname,std::string artist) const
{
    if (!music_view_art_song_) {
        ESP_LOGI(TAG, "music_view_art_song_ is null");
        return -1;
    }

    songname = NormalizeForSearch(songname);
    artist   = NormalizeForSearch(artist);

    char query[256];
    snprintf(query, sizeof(query), "%s-%s", artist.c_str(), songname.c_str());
    auto cmp = [](const void *k, const void *e){
        const char *key = (const char*)k;
        const MusicView *v = (const MusicView*)e;
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s-%s", v->artist_norm ? v->artist_norm : "", v->song_name ? v->song_name : "");
        return strcmp(key, tmp);
    };
    void *f = bsearch(query, music_view_art_song_, ps_music_count_, sizeof(MusicView), cmp);
    if (f) return ((MusicView*)f)->idx;

    // 预处理组合查询与 token
    std::string compact_q = artist + songname;
    const char* q_compact = compact_q.c_str();
    std::string orig_query = (artist.empty() ? songname : (artist + " " + songname));
    std::string q_token_norm = NormalizeForToken(orig_query);
    auto q_tokens = SplitTokensNoAlloc(q_token_norm);

    std::vector<int> freq_q(256, 0);
    {
        const unsigned char* pq = (const unsigned char*)q_compact;
        for (size_t k = 0; pq[k]; ++k) ++freq_q[pq[k]];
    }

    int best_idx = -1;
    int best_score = INT_MIN;
    int best_len_diff = INT_MAX;
    std::vector<int> freq_t(256);

    std::lock_guard<std::mutex> lock(music_library_mutex_);
    for (size_t i = 0; i < ps_music_count_; ++i) {
        const auto& m = ps_music_library_[i];
        if (!m.song_name) continue;

        int score = 0;

        if (!artist.empty() && m.artist_norm) {
            if (strstr(m.artist_norm, artist.c_str())) score += 150;
            if (IsSubsequence(artist.c_str(), m.artist_norm)) score += 80;
        }

        if (!songname.empty()) {
            if (strstr(m.song_name, songname.c_str())) score += 300;
            if (IsSubsequence(songname.c_str(), m.song_name)) score += 200;
        }

        const char* targ_token = m.token_norm ? m.token_norm : (m.file_name ? m.file_name : m.song_name);
        if (!q_tokens.empty() && targ_token && TokenSeqMatchUsingTokenNormNoAlloc(targ_token, q_tokens)) {
            score += 250;
        }

        const char* tgt_compact = m.song_name;
        if (tgt_compact && q_compact[0]) {
            if (strstr(tgt_compact, q_compact) != nullptr) score += 200;
        }

        // 字符重合度：组合 song_name + artist_norm
        std::fill(freq_t.begin(), freq_t.end(), 0);
        if (m.song_name) {
            const unsigned char* ps = (const unsigned char*)m.song_name;
            for (size_t k = 0; ps[k]; ++k) ++freq_t[ps[k]];
        }
        if (m.artist_norm) {
            const unsigned char* pa = (const unsigned char*)m.artist_norm;
            for (size_t k = 0; pa[k]; ++k) ++freq_t[pa[k]];
        }
        int qlen = (int)strlen(q_compact);
        int tgtlen = 0;
        if (m.song_name) tgtlen += strlen(m.song_name);
        if (m.artist_norm) tgtlen += strlen(m.artist_norm);
        score += OverlapScoreFromFreq(freq_q, freq_t, qlen);

        int len_diff = std::abs(qlen - tgtlen);
        score += std::max(0, 30 - len_diff);

        bool replace = false;
        if (score > best_score) replace = true;
        else if (score == best_score && len_diff < best_len_diff) replace = true;

        if (replace) {
            best_score = score;
            best_idx = static_cast<int>(i);
            best_len_diff = len_diff;
            if (best_score >= 900) break;
        }
    }

    if (best_idx >= 0) {
        ESP_LOGI(TAG, "best hit: %s - %s  score=%d",
                 ps_music_library_[best_idx].artist_norm ? ps_music_library_[best_idx].artist_norm : "<nil>",
                 ps_music_library_[best_idx].song_name ? ps_music_library_[best_idx].song_name : "<nil>",
                 best_score);
    } else {
        ESP_LOGW(TAG, "best hit not found for: %s - %s", artist.c_str(), songname.c_str());
    }
    return best_idx;
}






// 替换后的：按歌手随机/打分取 top5，使用模块化 helper
std::vector<int> Esp32Music::SearchMusicIndexBySingerRand5(std::string singer) const
{
    std::vector<int> res;
    if (!music_view_singer_ || ps_music_count_ == 0) return res;

    singer = NormalizeForSearch(singer);
    ESP_LOGI(TAG, "Searching for singer: %s", singer.c_str());

    auto cmp = [](const void *k, const void *e){
        return strcmp(((const MusicView*)k)->artist_norm,
                      ((const MusicView*)e)->artist_norm);
    };
    MusicView key{};
    key.artist_norm = singer.c_str();
    void *f = bsearch(&key, music_view_singer_, ps_music_count_,
                      sizeof(MusicView), cmp);

    if (f) {
        size_t left = (MusicView*)f - music_view_singer_;
        while (left > 0 &&
               strcmp(music_view_singer_[left - 1].artist_norm, singer.c_str()) == 0)
            --left;
        size_t right = left;
        while (right < ps_music_count_ &&
               strcmp(music_view_singer_[right].artist_norm, singer.c_str()) == 0)
            ++right;

        size_t cnt = right - left;
        if (cnt == 0) return res;

        std::vector<uint16_t> pool;
        pool.reserve(cnt);
        for (size_t i = left; i < right; ++i) pool.push_back(music_view_singer_[i].idx);

        static bool rand_init = false;
        if (!rand_init) { srand((unsigned)time(NULL)); rand_init = true; }
        if (cnt <= 5) res.assign(pool.begin(), pool.end());
        else {
            for (size_t i = cnt - 1; i > 0; --i) {
                size_t j = rand() % (i + 1);
                std::swap(pool[i], pool[j]);
            }
            res.assign(pool.begin(), pool.begin() + 5);
        }
        return res;
    }

    using Item = std::pair<int,int>;
    auto pq_cmp = [](const Item& a, const Item& b){ return a.first > b.first; };
    std::priority_queue<Item, std::vector<Item>, decltype(pq_cmp)> pq(pq_cmp);

    const char* q = singer.c_str();

    std::lock_guard<std::mutex> lock(music_library_mutex_);
    for (size_t i = 0; i < ps_music_count_; ++i) {
        const auto& mv = music_view_singer_[i];
        const char* artist_norm = mv.artist_norm;
        if (!artist_norm) continue;

        int score = 0;
        if (strstr(artist_norm, q) != nullptr) score += 200;
        if (IsSubsequence(q, artist_norm)) score += 150;

        const PSMusicInfo &mi = ps_music_library_[mv.idx];
        const char* token_src = mi.token_norm ? mi.token_norm : (mi.file_name ? mi.file_name : mi.artist);
        if (token_src && q && *q) {
            if (TokenSeqMatchUsingTokenNormNoAlloc(token_src, SplitTokensNoAlloc(std::string(q)))) score += 180;
        }

        int d = levenshtein_threshold(q, artist_norm, 4);
        if (d >= 0 && d <= 2) score += 80 - d * 20;

        if (score <= 0) continue;
        int lib_idx = static_cast<int>(mv.idx);
        pq.emplace(score, lib_idx);
        if (pq.size() > 5) pq.pop();
    }

    res.reserve(pq.size());
    while (!pq.empty()) {
        res.push_back(pq.top().second);
        pq.pop();
    }
    std::reverse(res.begin(), res.end());
    return res;
}


std::string Esp32Music::GetCurrentSongName()
{
    return current_song_name_;
}

void Esp32Music::LoadPlaybackPosition() {
    Settings settings("music", false);
    int idx = settings.GetInt("last_play_index", -1);
    int32_t ms = settings.GetInt("last_play_ms", 0);
    int64_t offset_i64 = settings.GetInt64("lastfileoffset", 0);
    auto music_name = settings.GetString("last_music_name", "");
    size_t offset = offset_i64 > 0 ? static_cast<size_t>(offset_i64) : 0;

    saved_play_index_ = idx;
    saved_play_ms_ = ms;
    saved_file_offset_ = offset;
    has_saved_MusicPosition_ = true;
    current_playlist_name_ = default_musiclist_;
    
    size_t pos = music_name.find_last_of("/\\");
    if (pos == std::string::npos) 
        current_song_name_ = music_name;
    else
        current_song_name_ = music_name.substr(pos + 1);

    play_index_ = saved_play_index_;
        ESP_LOGI(TAG, "Loaded saved playback pos: playlist='%s' name='%s' index=%d ms=%d offset=%d",
             current_playlist_name_.c_str(), current_song_name_.c_str(), saved_play_index_, saved_play_ms_, saved_file_offset_
             );

}


void Esp32Music::SavePlaybackPosition() {
    
    size_t pos = current_song_name_.find_last_of("/\\");
    if (pos != std::string::npos) 
        current_song_name_ = current_song_name_.substr(pos + 1);
    pos = current_song_name_.find_last_of('.');
    if (pos != std::string::npos)
        current_song_name_ = current_song_name_.substr(0, pos);

    pos = current_song_name_.find_last_of("-");
    if(pos != std::string::npos)
    {
        saved_play_index_ = SearchMusicIndexFromlistByArtSong(current_song_name_.substr(pos+1),current_song_name_.substr(0,pos));
    }
    else
    {
        saved_play_index_ = SearchMusicIndexFromlist(current_song_name_);
    }

    // 读取 current_play_file_offset_
    size_t file_offset = 0;
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        file_offset = current_play_file_offset_;
    }
// 对齐到1KB边界（大多数MP3帧是1KB的倍数）
    size_t aligned_offset = (file_offset / 1024) * 1024;
    int64_t play_ms = current_play_time_ms_;
    current_play_file_offset_ = aligned_offset;
    Settings settings("music", true);
    settings.SetInt("last_play_index", saved_play_index_);
    settings.SetInt("last_play_ms", static_cast<int32_t>(play_ms));
    settings.SetInt64("lastfileoffset", (int64_t)aligned_offset);
    settings.SetString("last_music_name", current_song_name_);

    settings.Commit();

    ESP_LOGI(TAG, "Saved playback pos: name=%s index=%d ms=%d offset=%d ",
              current_song_name_.c_str(), saved_play_index_, play_ms, aligned_offset);
}

bool Esp32Music::ResumeSavedPlayback() {
    if (!has_saved_MusicPosition_) {
        ESP_LOGI(TAG, "No saved playback position to resume");
        return false;
    }

    std::string file_path;
    
    if (saved_play_index_ < 0 || saved_play_index_ >= ps_music_count_) {
        ESP_LOGW(TAG, "Saved play index out of range: %d", saved_play_index_);
        return false;
    }
    file_path = ps_music_library_[saved_play_index_].file_path;
    

    if(current_play_file_offset_ == 0)
    {    
        // 优先按字节偏移恢复
        if (saved_file_offset_ > 0) {
            ESP_LOGI(TAG, "Resuming '%s' at offset %d", file_path.c_str(), saved_file_offset_);
            return PlayFromSD(file_path,"", saved_file_offset_);
        }

        // 没有偏移但有时间，尝试按时间估算偏移
        if (saved_play_ms_ > 0) {
            ESP_LOGI(TAG, "Resuming '%s' at approx time %lld ms", file_path.c_str(), (long long)saved_play_ms_);
            size_t file_size = get_file_size(file_path);
            MusicFileInfo info = GetMusicInfo(file_path);
            int duration_ms = 0;
            if (info.duration > 0) duration_ms = info.duration * 1000;
            if (duration_ms > 0 && file_size > 0) {
                double ratio = double(saved_play_ms_) / double(duration_ms);
                if (ratio < 0) ratio = 0;
                if (ratio > 0.99) ratio = 0.99;
                size_t approx_offset = static_cast<size_t>(file_size * ratio);
                ESP_LOGI(TAG, "Approximated offset %d (file_size=%d duration_ms=%d)", approx_offset, file_size, duration_ms);
                return PlayFromSD(file_path, "", approx_offset);
            }
        }
    }
    else
    {
        ESP_LOGI(TAG, "Resuming '%s' at current offset %d", file_path.c_str(), current_play_file_offset_);
        return PlayFromSD(file_path,"", current_play_file_offset_);
    }
    // 否则从头开始播放
    ESP_LOGI(TAG, "Resume fallback: start from beginning of %s", file_path.c_str());

    return PlayFromSD(file_path, "", 0);
}

// 新增：带 start_offset 参数的 PlayFromSD（设置 start_play_offset_ 后调用现有 StartSDCardStreaming）
bool Esp32Music::PlayFromSD(const std::string& file_path, const std::string& song_name, size_t start_offset) {
    // 设置启动偏移（ReadFromSDCard 在打开文件后会 fseek）
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        start_play_offset_ = start_offset;
    }

    // 使用已有逻辑开始播放（原来的 PlayFromSD 会调用 StartSDCardStreaming）
    return PlayFromSD(file_path, song_name);
}

void Esp32Music::UpdateMusicRecordList(const std::string& artist, const std::string& song_name)
{

    // 决定索引（仍使用 play_index_ 优先）
    int idx = play_index_;
    if (current_playlist_name_ != default_musiclist_) {
        if (artist.empty()) idx = SearchMusicIndexFromlistByArtSong(song_name, artist);
        else idx = SearchMusicIndexFromlist(song_name);
    }

    // 新节点（尾插），不复制字符串，仅保存指针到已有 ps_music_library_ 字符串
    Music_Record_Info* node = new Music_Record_Info();
    node->index = idx;
    node->next = nullptr;
    node->last = nullptr;

    if (idx >= 0 && ps_music_library_ && static_cast<size_t>(idx) < ps_music_count_) {
        node->song_name = ps_music_library_[idx].song_name;   // 指向 PSRAM 内的 char*
        node->artist = ps_music_library_[idx].artist;
    } else {
        node->song_name = nullptr;
        node->artist = nullptr;
    }

    if (!music_record_) {
        music_record_ = node;
    } else {
        Music_Record_Info* tail = music_record_;
        while (tail->next) tail = tail->next;
        tail->next = node;
        node->last = tail;
    }

    NowNode = node;

    // 限长：删除头节点直到长度 <= kMaxRecent
    size_t count = 0;
    Music_Record_Info* cur = music_record_;
    while (cur) { ++count; cur = cur->next; }

    while (count > kMaxRecent) {
        Music_Record_Info* old_head = music_record_;
        music_record_ = old_head->next;
        if (music_record_) music_record_->last = nullptr;
        if (NowNode == old_head) {
            Music_Record_Info* new_tail = music_record_;
            while (new_tail && new_tail->next) new_tail = new_tail->next;
            NowNode = new_tail;
        }
        delete old_head;
        --count;
    }

    ESP_LOGI(TAG, "UpdateMusicRecordList: appended idx=%d recent_count=%u",
             idx, (unsigned)count);
}

//----------------------------------------------故事----------------------------------------------------


void Esp32Music::ScanAndLoadStory() {
    ESP_LOGI(TAG, "Initializing default playlists from SD card story library");
    {
        // 清理并扫描
        if (!ScanStoryLibrary("/sdcard/故事")) {
            ESP_LOGW(TAG, "ScanStoryLibrary failed or SD not ready");
        }
    }
    LoadStoryPlaybackPosition();
}


static std::string NormalizeForSearch_local(const std::string &s) {
    return NormalizeForSearch(s);
}



// 释放 PSRAM 中的故事索引
void Esp32Music::free_ps_story_index_locked() {
    if (!ps_story_index_) return;
    for (size_t i = 0; i < ps_story_count_; ++i) {
        PSStoryEntry &e = ps_story_index_[i];
        // 释放章节字符串（这些在 SPIRAM 分配）
        if (e.chapters) {
            for (size_t j = 0; j < e.chapter_count; ++j) {
                if (e.chapters[j]) heap_caps_free(e.chapters[j]);
            }
            heap_caps_free(e.chapters);
            e.chapters = nullptr;
            e.chapter_count = 0;
        }
        if (e.category) { ps_free_str(e.category); e.category = nullptr; }
        if (e.story_name) { ps_free_str(e.story_name); e.story_name = nullptr; }
        // 释放 token_norm（已放在 PSRAM）
        if (e.token_norm) { ps_free_str(e.token_norm); e.token_norm = nullptr; }
        // norm_* 是 std::string（DRAM），将在 delete[] 时自动析构
    }
    // ps_story_index_ 是用 new[] 分配的，使用 delete[]
    delete [] ps_story_index_;
    ps_story_index_ = nullptr;
    ps_story_count_ = 0;
    ps_story_capacity_ = 0;
}

// 可选的追加函数（要求在调用前持有 story_index_mutex_）
bool Esp32Music::ps_add_story_locked(const StoryEntry &e) {
    size_t need = ps_story_count_ + 1;
    if (need > ps_story_capacity_) {
        size_t new_cap = ps_story_capacity_ ? (ps_story_capacity_ * 3) / 2 : 16;
        if (new_cap < need) new_cap = need;
        PSStoryEntry *new_arr = new (std::nothrow) PSStoryEntry[new_cap];
        if (!new_arr) return false;
        if (ps_story_index_ && ps_story_count_ > 0) {
            for (size_t i = 0; i < ps_story_count_; ++i) {
                new_arr[i].category = ps_story_index_[i].category;
                new_arr[i].story_name = ps_story_index_[i].story_name;
                new_arr[i].chapters = ps_story_index_[i].chapters;
                new_arr[i].chapter_count = ps_story_index_[i].chapter_count;
                new_arr[i].norm_category = std::move(ps_story_index_[i].norm_category);
                new_arr[i].norm_story = std::move(ps_story_index_[i].norm_story);
                // prevent double-free
                ps_story_index_[i].category = nullptr;
                ps_story_index_[i].story_name = nullptr;
                ps_story_index_[i].chapters = nullptr;
                ps_story_index_[i].chapter_count = 0;
                ps_story_index_[i].token_norm = nullptr;
            }
            delete [] ps_story_index_;
        }
        ps_story_index_ = new_arr;
        ps_story_capacity_ = new_cap;
    }

    PSStoryEntry &dst = ps_story_index_[ps_story_count_];
    dst.category = ps_strdup(e.category);
    dst.story_name = ps_strdup(e.story);
    if (!dst.category || !dst.story_name) {
        ps_free_str(dst.category); dst.category = nullptr;
        ps_free_str(dst.story_name); dst.story_name = nullptr;
        return false;
    }

    if (!e.chapters.empty()) {
        dst.chapters = (char**)heap_caps_malloc(e.chapters.size() * sizeof(char*), MALLOC_CAP_SPIRAM);
        if (!dst.chapters) {
            ps_free_str(dst.category); dst.category = nullptr;
            ps_free_str(dst.story_name); dst.story_name = nullptr;
            return false;
        }
        memset(dst.chapters, 0, e.chapters.size() * sizeof(char*));
        for (size_t i = 0; i < e.chapters.size(); ++i) {
            dst.chapters[i] = ps_strdup(e.chapters[i]);
            if (!dst.chapters[i]) {
                for (size_t k = 0; k < i; ++k) heap_caps_free(dst.chapters[k]);
                heap_caps_free(dst.chapters);
                dst.chapters = nullptr;
                ps_free_str(dst.category); dst.category = nullptr;
                ps_free_str(dst.story_name); dst.story_name = nullptr;
                return false;
            }
        }
        dst.chapter_count = e.chapters.size();
    } else {
        dst.chapters = nullptr;
        dst.chapter_count = 0;
    }

    dst.norm_category = NormalizeForSearch_local(std::string(dst.category ? dst.category : ""));
    dst.norm_story = NormalizeForSearch_local(std::string(dst.story_name ? dst.story_name : ""));

    // 生成并保存 token_norm（放到 PSRAM，供模糊搜索时无分配访问）
    std::string token_src;
    token_src.reserve(256);
    if (dst.category) { token_src += std::string(dst.category); token_src += ' '; }
    if (dst.story_name) { token_src += std::string(dst.story_name); token_src += ' '; }
    // 仅加入章节名（文件名部分，不包含路径与扩展）以减少大小
    for (size_t i = 0; i < dst.chapter_count; ++i) {
        if (!dst.chapters[i]) continue;
        std::string chap = dst.chapters[i];
        size_t p = chap.find_last_of("/\\");
        if (p != std::string::npos) chap = chap.substr(p + 1);
        size_t dot = chap.find_last_of('.');
        if (dot != std::string::npos) chap = chap.substr(0, dot);
        token_src += chap;
        token_src += ' ';
    }
    std::string token_norm = NormalizeForToken(token_src);
    dst.token_norm = ps_strdup(token_norm);
    if (!dst.token_norm) {
        // 回滚已分配资源
        if (dst.chapters) {
            for (size_t j = 0; j < dst.chapter_count; ++j) if (dst.chapters[j]) heap_caps_free(dst.chapters[j]);
            heap_caps_free(dst.chapters);
            dst.chapters = nullptr;
            dst.chapter_count = 0;
        }
        ps_free_str(dst.category); dst.category = nullptr;
        ps_free_str(dst.story_name); dst.story_name = nullptr;
        return false;
    }

    ps_story_count_++;
    return true;
}
bool Esp32Music::ScanStoryLibrary(const std::string& story_folder) {
    ESP_LOGI(TAG, "Scanning story library from: %s", story_folder.c_str());
    struct stat st;
    if (stat(story_folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "Story folder not found: %s", story_folder.c_str());
        return false;
    }

    // 先清理旧的 PSRAM 索引
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        free_ps_story_index_locked();
    }

    DIR* d_cat = opendir(story_folder.c_str());
    if (!d_cat) {
        ESP_LOGW(TAG, "Failed to open story folder: %s", story_folder.c_str());
        return false;
    }

    struct dirent* ent_cat;
    size_t added = 0;

    while ((ent_cat = readdir(d_cat)) != nullptr) {
        const char* cname = ent_cat->d_name;
        if (strcmp(cname, ".") == 0 || strcmp(cname, "..") == 0) continue;

        std::string cat_path = story_folder + "/" + cname;
        struct stat st_cat;
        if (stat(cat_path.c_str(), &st_cat) != 0 || !S_ISDIR(st_cat.st_mode)) continue;

        DIR* d_story = opendir(cat_path.c_str());
        if (!d_story) continue;
        struct dirent* ent_story;

        while ((ent_story = readdir(d_story)) != nullptr) {
            const char* sname = ent_story->d_name;
            if (strcmp(sname, ".") == 0 || strcmp(sname, "..") == 0) continue;

            std::string story_path = cat_path + "/" + sname;
            struct stat st_story;
            if (stat(story_path.c_str(), &st_story) != 0 || !S_ISDIR(st_story.st_mode)) continue;

            // 收集章节文件到临时容器
            std::vector<std::string> chapters;
            DIR* d_ch = opendir(story_path.c_str());
            if (d_ch) {
                struct dirent* ent_ch;
                while ((ent_ch = readdir(d_ch)) != nullptr) {
                    const char* chname = ent_ch->d_name;
                    if (strcmp(chname, ".") == 0 || strcmp(chname, "..") == 0) continue;
                    std::string ch_full = story_path + "/" + chname;
                    struct stat st_ch;
                    if (stat(ch_full.c_str(), &st_ch) == 0 && S_ISREG(st_ch.st_mode)) {
                        if (IsMusicFile(ch_full)) chapters.push_back(ch_full);
                    }
                }
                closedir(d_ch);
            }

            if (chapters.empty()) continue;
            std::sort(chapters.begin(), chapters.end());

            // 构造临时 StoryEntry 并追加到 PSRAM（短时间持锁）
            StoryEntry se;
            se.category = cname;
            se.story = sname;
            se.chapters = std::move(chapters);
            se.norm_category = NormalizeForSearch_local(se.category);
            se.norm_story = NormalizeForSearch_local(se.story);

            {
                std::lock_guard<std::mutex> lock(story_index_mutex_);
                if (ps_add_story_locked(se)) {
                    added++;
                } else {
                    ESP_LOGW(TAG, "Failed to add story to PSRAM: %s / %s", se.category.c_str(), se.story.c_str());
                    // 继续扫描其它故事
                }
            }
        }
        closedir(d_story);
    }

    closedir(d_cat);

    ESP_LOGI(TAG, "Story library scan completed, entries=%u", (unsigned)ps_story_count_);

    return ps_story_count_ > 0;
}
std::vector<std::string> Esp32Music::GetStoryCategories() const {
    std::vector<std::string> cats;
    std::lock_guard<std::mutex> lock(story_index_mutex_);
    for (size_t i = 0; i < ps_story_count_; ++i) {
        const char* c = ps_story_index_[i].category;
        if (c) {
            std::string s(c);
            if (std::find(cats.begin(), cats.end(), s) == cats.end()) cats.push_back(s);
        }
    }
    return cats;
}


std::vector<std::string> Esp32Music::GetStoriesInCategory(const std::string& category) const {
    std::string norm = NormalizeForSearch_local(category);
    std::vector<std::string> list;
    if (norm.empty()) return list;

    // 收集匹配该类别的索引（线程安全）
    std::vector<size_t> matches;
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            if (ps_story_index_[i].norm_category == norm) {
                if (ps_story_index_[i].story_name) matches.push_back(i);
            }
        }
    }

    if (matches.empty()) return list;

    // 若匹配多于5个，随机打乱并选前5个；否则返回全部
    size_t take = matches.size() > 5 ? 5 : matches.size();
    if (matches.size() > take) {
        for (size_t i = matches.size() - 1; i > 0; --i) {
            size_t j = (size_t)(esp_random() % (i + 1));
            if (i != j) std::swap(matches[i], matches[j]);
        }
    }

    list.reserve(take);
    for (size_t k = 0; k < take; ++k) {
        const char* sn = ps_story_index_[matches[k]].story_name;
        if (sn) list.emplace_back(std::string(sn));
    }
    return list;
}

std::vector<std::string> Esp32Music::GetChaptersForStory(const std::string& category, const std::string& story_name) const {
    std::string ncat = NormalizeForSearch_local(category);
    std::string q_norm = NormalizeForSearch_local(story_name);
    std::vector<std::string> chs;
    if (ncat.empty() || q_norm.empty()) return chs;

    // 预计算 query 频率向量
    std::vector<int> freq_q(256, 0);
    {
        const unsigned char* pq = (const unsigned char*)q_norm.c_str();
        for (size_t k = 0; pq[k]; ++k) ++freq_q[pq[k]];
    }

    int best_score = INT_MIN;
    size_t best_idx = SIZE_MAX;
    std::lock_guard<std::mutex> lock(story_index_mutex_);
    // 优先尝试精确匹配（恢复原行为）
    for (size_t i = 0; i < ps_story_count_; ++i) {
        const PSStoryEntry &e = ps_story_index_[i];
        if (e.norm_category != ncat) continue;
        if (e.norm_story == q_norm) {
            // 直接返回该故事的章节
            for (size_t j = 0; j < e.chapter_count; ++j) {
                if (e.chapters && e.chapters[j]) chs.emplace_back(e.chapters[j]);
            }
            return chs;
        }
    }

    // 精确未命中 -> 进行模糊匹配（只在同类别下搜索）
    for (size_t i = 0; i < ps_story_count_; ++i) {
        const PSStoryEntry &e = ps_story_index_[i];
        if (e.norm_category != ncat) continue;
        if (!e.story_name) continue;

        const std::string &norm_story = e.norm_story;
        const char* story_c = norm_story.c_str();

        int score = 0;
        // 包含子串优先
        if (story_c && strstr(story_c, q_norm.c_str())) score += 400;
        // 子序列匹配
        if (IsSubsequence(q_norm.c_str(), story_c)) score += 200;
        // token 顺序匹配（利用已存在于 PSRAM 的 token_norm）
        const char* targ_token = e.token_norm;
        if (targ_token) {
            auto q_tokens = SplitTokensNoAlloc(NormalizeForToken(story_name));
            if (!q_tokens.empty() && TokenSeqMatchUsingTokenNormNoAlloc(targ_token, q_tokens)) score += 300;
        }
        // 字符重合度
        std::vector<int> freq_t(256, 0);
        ComputeFreqVector(story_c, freq_t);
        int qlen = (int)strlen(q_norm.c_str());
        score += OverlapScoreFromFreq(freq_q, freq_t, qlen);

        // 编辑距离保守加分
        int d = levenshtein_threshold(q_norm.c_str(), story_c, 6);
        if (d >= 0 && d <= 3) score += std::max(0, 80 - d * 20);

        // 若章节名中包含 query，也加分（章节文件名已存于 PSRAM）
        for (size_t j = 0; j < e.chapter_count; ++j) {
            const char* chap = e.chapters && e.chapters[j] ? e.chapters[j] : nullptr;
            if (!chap) continue;
            // 仅检测文件名部分（去掉路径与扩展）
            std::string chapname = chap;
            size_t p = chapname.find_last_of("/\\");
            if (p != std::string::npos) chapname = chapname.substr(p + 1);
            size_t dot = chapname.find_last_of('.');
            if (dot != std::string::npos) chapname = chapname.substr(0, dot);
            std::string norm_ch = NormalizeForSearch_local(chapname);
            if (!norm_ch.empty() && strstr(norm_ch.c_str(), q_norm.c_str())) score += 180;
            if (IsSubsequence(q_norm.c_str(), norm_ch.c_str())) score += 80;
        }

        // 选择最高分，平分时选择故事名更短的（更精确）
        bool replace = false;
        if (score > best_score) replace = true;
        else if (score == best_score && norm_story.size() < (best_idx == SIZE_MAX ? SIZE_MAX : ps_story_index_[best_idx].norm_story.size()))
            replace = true;

        if (replace) {
            best_score = score;
            best_idx = i;
        }
    }

    // 如果找到有效模糊匹配，则返回其章节列表
    if (best_idx != SIZE_MAX && best_score > 0) {
        const PSStoryEntry &best = ps_story_index_[best_idx];
        for (size_t j = 0; j < best.chapter_count; ++j) {
            if (best.chapters && best.chapters[j]) chs.emplace_back(best.chapters[j]);
        }
    }
    return chs;
}



size_t Esp32Music::FindStoryIndexFuzzy(const std::string& story_name) const {
    std::string q_norm = NormalizeForSearch_local(story_name);
    if (q_norm.empty()) return SIZE_MAX;

    // 先快速精确匹配（规范化后）
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            const PSStoryEntry &e = ps_story_index_[i];
            if (e.norm_story == q_norm) return i;
        }
    }

    // 预处理 token 与频率向量
    std::string q_token_norm = NormalizeForToken(story_name);
    auto q_tokens = SplitTokensNoAlloc(q_token_norm);

    std::vector<int> freq_q(256, 0);
    {
        const unsigned char* pq = (const unsigned char*)q_norm.c_str();
        for (size_t k = 0; pq[k]; ++k) ++freq_q[pq[k]];
    }

    int best_score = INT_MIN;
    size_t best_idx = SIZE_MAX;

    std::lock_guard<std::mutex> lock(story_index_mutex_);
    for (size_t i = 0; i < ps_story_count_; ++i) {
        const PSStoryEntry &e = ps_story_index_[i];
        if (!e.story_name) continue;

        const std::string &norm_story = e.norm_story;
        const char* story_c = norm_story.c_str();

        int score = 0;

        // 直接子串与子序列匹配
        if (story_c && strstr(story_c, q_norm.c_str())) score += 400;
        if (IsSubsequence(q_norm.c_str(), story_c)) score += 200;

        // token 顺序匹配（利用 PSRAM 中的 token_norm）
        const char* targ_token = e.token_norm;
        if (targ_token && !q_tokens.empty() && TokenSeqMatchUsingTokenNormNoAlloc(targ_token, q_tokens)) score += 300;

        // 字符重合度
        std::vector<int> freq_t(256, 0);
        ComputeFreqVector(story_c, freq_t);
        int qlen = (int)strlen(q_norm.c_str());
        score += OverlapScoreFromFreq(freq_q, freq_t, qlen);

        // 编辑距离保守加分
        int d = levenshtein_threshold(q_norm.c_str(), story_c, 6);
        if (d >= 0 && d <= 3) score += std::max(0, 80 - d * 20);

        // 章节名匹配也加分
        for (size_t j = 0; j < e.chapter_count; ++j) {
            const char* chap = e.chapters && e.chapters[j] ? e.chapters[j] : nullptr;
            if (!chap) continue;
            std::string chapname = chap;
            size_t p = chapname.find_last_of("/\\");
            if (p != std::string::npos) chapname = chapname.substr(p + 1);
            size_t dot = chapname.find_last_of('.');
            if (dot != std::string::npos) chapname = chapname.substr(0, dot);
            std::string norm_ch = NormalizeForSearch_local(chapname);
            if (!norm_ch.empty() && strstr(norm_ch.c_str(), q_norm.c_str())) score += 180;
            if (IsSubsequence(q_norm.c_str(), norm_ch.c_str())) score += 80;
        }

        // 记录最佳（平分时倾向于更短的故事名）
        bool replace = false;
        if (score > best_score) replace = true;
        else if (score == best_score && best_idx != SIZE_MAX) {
            if (norm_story.size() < ps_story_index_[best_idx].norm_story.size()) replace = true;
        } else if (best_idx == SIZE_MAX && score > INT_MIN) replace = true;

        if (replace) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx != SIZE_MAX && best_score > 0) {
        ESP_LOGI(TAG, "FindStoryIndexFuzzy: best hit idx=%u score=%d (%s)", (unsigned)best_idx, best_score,
                 ps_story_index_[best_idx].story_name ? ps_story_index_[best_idx].story_name : "<nil>");
        return best_idx;
    }
    return SIZE_MAX;
}

size_t Esp32Music::FindStoryIndexInCategory(const std::string& category, const std::string& story_name) const {
    std::string ncat = NormalizeForSearch_local(category);
    std::string q_norm = NormalizeForSearch_local(story_name);
    if (ncat.empty() || q_norm.empty()) return -1;

    // 预生成 token 查询
    std::string q_token_norm = NormalizeForToken(story_name);
    auto q_tokens = SplitTokensNoAlloc(q_token_norm);

    // 先做快速的精确匹配（规范化后）
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            const PSStoryEntry &e = ps_story_index_[i];
            if (e.norm_category != ncat) continue;
            if (e.norm_story == q_norm) return i; // 精确命中，立即返回
        }
    }

    // 预计算 query 的字节频率（用于重合度评分）
    std::vector<int> freq_q(256, 0);
    {
        const unsigned char* pq = (const unsigned char*)q_norm.c_str();
        for (size_t k = 0; pq[k]; ++k) ++freq_q[pq[k]];
    }

    int best_total = INT_MIN;      // 总分（以 story 优先权放大）
    int best_story_score = INT_MIN;
    size_t best_idx = SIZE_MAX;

    std::lock_guard<std::mutex> lock(story_index_mutex_);
    for (size_t i = 0; i < ps_story_count_; ++i) {
        const PSStoryEntry &e = ps_story_index_[i];
        if (e.norm_category != ncat) continue;
        if (!e.story_name) continue;

        const std::string &norm_story = e.norm_story;
        const char* story_c = norm_story.c_str();

        int story_score = 0;
        int chapter_score = 0;

        // ========== story 相关评分（优先级高） ==========
        if (story_c && strstr(story_c, q_norm.c_str())) story_score += 400;
        if (IsSubsequence(q_norm.c_str(), story_c)) story_score += 200;

        const char* targ_token = e.token_norm;
        if (targ_token && !q_tokens.empty() && TokenSeqMatchUsingTokenNormNoAlloc(targ_token, q_tokens)) story_score += 300;

        // story 字符重合度
        std::vector<int> freq_t(256, 0);
        ComputeFreqVector(story_c, freq_t);
        int qlen = (int)strlen(q_norm.c_str());
        story_score += OverlapScoreFromFreq(freq_q, freq_t, qlen);

        int d = levenshtein_threshold(q_norm.c_str(), story_c, 6);
        if (d >= 0 && d <= 3) story_score += std::max(0, 80 - d * 20);

        // ========== chapter 相关评分（次要） ==========
        for (size_t j = 0; j < e.chapter_count; ++j) {
            const char* chap = e.chapters && e.chapters[j] ? e.chapters[j] : nullptr;
            if (!chap) continue;
            std::string chapname = chap;
            size_t p = chapname.find_last_of("/\\");
            if (p != std::string::npos) chapname = chapname.substr(p + 1);
            size_t dot = chapname.find_last_of('.');
            if (dot != std::string::npos) chapname = chapname.substr(0, dot);
            std::string norm_ch = NormalizeForSearch_local(chapname);
            if (norm_ch.empty()) continue;
            if (strstr(norm_ch.c_str(), q_norm.c_str())) chapter_score += 180;
            if (IsSubsequence(q_norm.c_str(), norm_ch.c_str())) chapter_score += 80;

            // 章节和 query 的重合度（小权重）
            std::vector<int> freq_ch(256, 0);
            ComputeFreqVector(norm_ch.c_str(), freq_ch);
            chapter_score += OverlapScoreFromFreq(freq_q, freq_ch, qlen) / 4; // 降低权重
        }

        // 将 story_score 放大以保证“故事匹配优先”，再加上 chapter_score 作为次要因子
        int total = story_score * 1000 + chapter_score;

        // 选择最佳：优先 total，然后在 story_score 平局时选短名
        bool replace = false;
        if (total > best_total) replace = true;
        else if (total == best_total && best_idx != SIZE_MAX) {
            if (norm_story.size() < ps_story_index_[best_idx].norm_story.size()) replace = true;
        }

        if (replace) {
            best_total = total;
            best_story_score = story_score;
            best_idx = i;
        }
    }

    if (best_idx != SIZE_MAX && best_total > 0) {
        ESP_LOGI(TAG, "FindStoryIndexInCategory: chosen idx=%u total=%d story_score=%d (%s / %s)",
                 (unsigned)best_idx, best_total, best_story_score,
                 ps_story_index_[best_idx].category ? ps_story_index_[best_idx].category : "<nil>",
                 ps_story_index_[best_idx].story_name ? ps_story_index_[best_idx].story_name : "<nil>");
        return best_idx;
    }
    return -1;
}

bool Esp32Music::SelectStoryAndPlay() {
    std::string ncat = NormalizeForSearch_local(current_category_name_);
    std::string nst = NormalizeForSearch_local(current_story_name_);
    PSStoryEntry found;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            auto &e = ps_story_index_[i];
            if (e.norm_category == ncat && e.norm_story == nst) {
                found = e; 
                ok = true;
                break;
            }
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "SelectStoryAndPlay: story not found '%s' / '%s'", ncat.c_str(), nst.c_str());
        return false;
    }
    if (found.chapter_count == 0 || !found.chapters) {
        ESP_LOGW(TAG, "SelectStoryAndPlay: story has no chapters '%s' / '%s'", ncat.c_str(), nst.c_str());
        return false;
    }
    if (current_chapter_index_ >= found.chapter_count) current_chapter_index_ = 0;
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        current_story_name_ = found.story_name ? std::string(found.story_name) : std::string();
    }


    return PlayFromSD(std::string(found.chapters[current_chapter_index_]), current_story_name_);
}


// ===== 故事断点播放实现 =====
void Esp32Music::SaveStoryPlaybackPosition() {
    // 读取当前播放偏移（从 current_play_file_offset_）
    size_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        offset = current_play_file_offset_;
    }
    int ms = static_cast<int>(current_play_time_ms_);
    Settings settings("stories", true);
    settings.SetString("last_category", current_category_name_.c_str());
    settings.SetString("last_story", current_story_name_.c_str());
    settings.SetInt("last_chapter", current_chapter_index_);
    settings.SetInt64("last_chptoffset", offset);
    settings.SetInt("last_chpt_ms", ms);
    settings.Commit();

    ESP_LOGI(TAG, "Saved story playback pos: category=%s story=%s chapter=%d offset=%d ms=%d",
             current_category_name_.c_str(),current_story_name_.c_str(),current_chapter_index_+1,offset,ms);
}

void Esp32Music::LoadStoryPlaybackPosition() {
    Settings settings("stories", false);
    std::string cat = settings.GetString("last_category", "");
    std::string name = settings.GetString("last_story", "");
    int idx = settings.GetInt("last_chapter", -1);
    int64_t offset_i64 = settings.GetInt64("last_chptoffset", 0);
    int ms = settings.GetInt("last_chpt_ms", 0);

    saved_story_category_ = cat;
    saved_story_name_ = name;
    saved_chapter_index_ = idx;
    saved_chapter_file_offset_ = offset_i64 > 0 ? static_cast<uint64_t>(offset_i64) : 0;
    saved_chapter_ms_ = ms;
    has_saved_story_position_ = (!saved_story_category_.empty() && !saved_story_name_.empty());

    current_category_name_ = saved_story_category_;
    current_chapter_index_ = saved_chapter_index_;
    current_story_name_ = saved_story_name_;
    ESP_LOGI(TAG, "Loaded saved story pos: category='%s' story='%s' chapter=%d offset=%llu ms=%d",
             current_category_name_.c_str(), current_story_name_.c_str(), current_chapter_index_+1,
             (unsigned long long)saved_chapter_file_offset_, saved_chapter_ms_);
}

bool Esp32Music::ResumeSavedStoryPlayback() {
    if (!has_saved_story_position_) {
        ESP_LOGI(TAG, "No saved story playback position to resume");
        return false;
    }

    // 找到故事索引并定位章节
    std::string ncat = NormalizeForSearch_local(saved_story_category_);
    std::string nst = NormalizeForSearch_local(saved_story_name_);
    size_t found_index = SIZE_MAX;
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            if (ps_story_index_[i].norm_category == ncat && ps_story_index_[i].norm_story == nst) {
                found_index = i;
                break;
            }
        }
        if (found_index == SIZE_MAX) {
            ESP_LOGW(TAG, "Saved story not found in index: %s / %s", saved_story_category_.c_str(), saved_story_name_.c_str());
            return false;
        }
        // 校验章节索引
        int chapter_idx = saved_chapter_index_;
        if (chapter_idx < 0 || static_cast<size_t>(chapter_idx) >= ps_story_index_[found_index].chapter_count) {
            ESP_LOGW(TAG, "Saved chapter index out of range, fallback to 0");
            chapter_idx = 0;
        }
        const char* chapter_path = ps_story_index_[found_index].chapters[chapter_idx];
        if (!chapter_path) {
            ESP_LOGW(TAG, "Saved chapter path null");
            return false;
        }

        // 开始播放，优先按字节偏移恢复
        if (saved_chapter_file_offset_ > 0) {
            ESP_LOGI(TAG, "Resuming story '%s'/%s chapter %d at offset %llu",
                     saved_story_category_.c_str(), saved_story_name_.c_str(), chapter_idx+1, (unsigned long long)saved_chapter_file_offset_);
            // 设置当前story信息，便于显示/保存
            current_story_name_ = ps_story_index_[found_index].story_name ? std::string(ps_story_index_[found_index].story_name) : saved_story_name_;
            MusicOrStory_ = STORY;
            current_chapter_index_ = chapter_idx;
            current_category_name_ = saved_story_category_;
            current_story_name_ = saved_story_name_;
            return PlayFromSD(std::string(chapter_path), current_story_name_, (size_t)saved_chapter_file_offset_);
        }

        // 若有 ms 信息且文件大小与时长能估算偏移，则可在此扩展（目前直接从头开始或按 offset）
        if (saved_chapter_ms_ > 0) {
            ESP_LOGI(TAG, "Resuming story by ms (%d) not implemented estimation, falling back to start", saved_chapter_ms_);
        }

        // 否则从头开始播放
        ESP_LOGI(TAG, "Resuming story from beginning: %s / %s chapter %d", saved_story_category_.c_str(), saved_story_name_.c_str(), chapter_idx);
        current_story_name_ = ps_story_index_[found_index].story_name ? std::string(ps_story_index_[found_index].story_name) : saved_story_name_;
        MusicOrStory_ = STORY;
        current_chapter_index_ = chapter_idx;
        current_category_name_ = saved_story_category_;
        current_story_name_ = current_story_name_;
        return PlayFromSD(std::string(chapter_path), current_story_name_, 0);
    }
}


bool Esp32Music::NextChapterInStory(const std::string& category, const std::string& story_name) {
    // 查找 story 在 PSRAM 索引中的位置
    std::string ncat = NormalizeForSearch_local(category);
    std::string nst = NormalizeForSearch_local(story_name);

    size_t found_index = SIZE_MAX;
    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            if (ps_story_index_[i].norm_category == ncat && ps_story_index_[i].norm_story == nst) {
                found_index = i;
                break;
            }
        }
    }

    if (found_index == SIZE_MAX) {
        ESP_LOGW(TAG, "NextChapterInStory: story not found '%s' / '%s'", category.c_str(), story_name.c_str());
        return false;
    }

    // 计算下一个章节索引（如果越界则尝试切换到同类别的下一个故事）
    int next_idx = 0;
    {
        {
            std::lock_guard<std::mutex> lock(story_index_mutex_);
            if (!current_story_name_.empty() && current_story_name_ == (ps_story_index_[found_index].story_name ? std::string(ps_story_index_[found_index].story_name) : std::string())) {
                next_idx = current_chapter_index_ + 1;
                ESP_LOGI(TAG, "NextChapterInStory: current chapter index %d, next %d", current_chapter_index_+1, next_idx+1);
            } else {
                ESP_LOGI(TAG, "Cant Find Story:%s",current_story_name_.c_str());
                next_idx = 0;
            }
        }
        if (next_idx < 0) next_idx = 0;
        //越界了
        if (static_cast<size_t>(next_idx) >= ps_story_index_[found_index].chapter_count) {
            
            if(StoryPlayback_mode_ == PLAYBACK_MODE_LOOP)
            {
                ESP_LOGI(TAG,"为你循环播放");
                next_idx = 0;
            }
            else
            {
                ESP_LOGI(TAG,"为你播放下一个故事");
                return NextStoryInCategory(category);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        const char* p = ps_story_index_[found_index].chapters[next_idx];
        if (!p) {
            ESP_LOGW(TAG, "NextChapterInStory: chapter path null for index %d", next_idx);
            return false;
        }

        current_category_name_ = category;
        current_chapter_index_ = next_idx;
        current_story_name_ = story_name;
    }

    return true;

}


bool Esp32Music::NextStoryInCategory(const std::string& category) {
    // 目标类别（若传入为空则使用当前类别）
    std::string use_cat = category.empty() ? current_category_name_ : category;
    std::string ncat = NormalizeForSearch_local(use_cat);
    std::string ncurr = NormalizeForSearch_local(current_story_name_);

    size_t first_in_cat = SIZE_MAX;
    size_t curr_index = SIZE_MAX;

    {
        //找到目标类别和故事
        std::lock_guard<std::mutex> lock(story_index_mutex_);
        for (size_t i = 0; i < ps_story_count_; ++i) {
            if (ps_story_index_[i].norm_category != ncat) continue;
            if (first_in_cat == SIZE_MAX) first_in_cat = i;
            if (ncurr.size() && ps_story_index_[i].norm_story == ncurr) {
                curr_index = i;
                break;
            }
        }

        // 如果类别中没有任何故事
        if (first_in_cat == SIZE_MAX) {
            ESP_LOGW(TAG, "NextStoryInCategory: no stories in category '%s'", use_cat.c_str());
            return false;
        }

        // 选择下一个故事索引：如果当前故事在列表中，则从其后开始查找；否则选择类别第一个
        size_t next_story = SIZE_MAX;
        if (curr_index == SIZE_MAX) {
            next_story = first_in_cat;
        } else {
            if(StoryPlayback_mode_ == PLAYBACK_MODE_ORDER) {
                // 向后查找同类别的下一个故事，遇到末尾则从头开始查找（同类别）
                for (size_t i = curr_index + 1; i < ps_story_count_; ++i) {
                    if (ps_story_index_[i].norm_category == ncat) { next_story = i; break; }
                }
                if (next_story == SIZE_MAX) {
                    for (size_t i = 0; i < curr_index; ++i) {
                        if (ps_story_index_[i].norm_category == ncat) { next_story = i; break; }
                    }
                }
            } else if(StoryPlayback_mode_ == PLAYBACK_MODE_RANDOM) {
                // 随机选择同类别的故事，直到选到与当前不同的故事为止
                size_t i=0;
                while( i < ps_story_count_) {
                    i=esp_random() % ps_story_count_;
                    if ((ps_story_index_[i].norm_category == ncat) && (ps_story_index_[i].story_name!=current_story_name_)) { next_story = i; break; }
                }
            }
        }

        if (next_story == SIZE_MAX) {
            // 在整个库中随机选择一个有章节的故事

            size_t pick = esp_random() % ps_story_count_;
            if(ps_story_index_[pick].chapter_count > 0 && ps_story_index_[pick].chapters)
            {
                next_story = pick;
            }
            else
            {
                //继续随机直到找到有章节的故事
                while( pick < ps_story_count_) {
                    pick=esp_random() % ps_story_count_;
                    if (ps_story_index_[pick].chapter_count > 0 && ps_story_index_[pick].chapters) { next_story = pick; break; }
                }
            }
            ESP_LOGI(TAG, "NextStoryInCategory: randomized pick index %u across all stories", (unsigned)next_story);
        }

        // 确认目标故事有章节
        if (ps_story_index_[next_story].chapter_count == 0 || !ps_story_index_[next_story].chapters) {
            ESP_LOGW(TAG, "NextStoryInCategory: target story has no chapters '%s' / '%s'",
                     ps_story_index_[next_story].category ? ps_story_index_[next_story].category : "<nil>",
                     ps_story_index_[next_story].story_name ? ps_story_index_[next_story].story_name : "<nil>");
            return false;
        }


        current_category_name_ = ps_story_index_[next_story].category ? std::string(ps_story_index_[next_story].category) : std::string();
        current_story_name_ = ps_story_index_[next_story].story_name ? std::string(ps_story_index_[next_story].story_name) : std::string();
        current_chapter_index_ = 0;

        return true;
    }
}

const PSStoryEntry* Esp32Music::GetStoryLibrary(size_t &out_count) const
{
    std::lock_guard<std::mutex> lock(story_index_mutex_);
    out_count = ps_story_count_;
    return ps_story_index_;

}




 void Esp32Music::SetCurrentCategoryName(const std::string& category)
 {
    current_category_name_ = category;
    ESP_LOGI(TAG,"Current Catgory:%s",current_category_name_.c_str());
}

 void Esp32Music::SetCurrentStoryName(const std::string& story)
 {
    current_story_name_ = story;
    ESP_LOGI(TAG,"Current Story:%s",current_story_name_.c_str());
}

 void Esp32Music::SetCurrentChapterIndex(int index)
 {
       
    current_chapter_index_ = index;
    ESP_LOGI(TAG,"Current Index:%d",current_chapter_index_+1);
}

