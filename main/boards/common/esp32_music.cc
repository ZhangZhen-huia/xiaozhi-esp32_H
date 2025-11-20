#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "display/lcd_display.h"
#include <esp_log.h>
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
#define TAG "Esp32Music"

// ========== 简单的ESP32认证函数 ==========
/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id() {
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥
 * @param timestamp 时间戳
 * @return 动态密钥字符串
 */
static std::string generate_dynamic_key(int64_t timestamp) {
    // 密钥（请修改为与服务端一致）
    const std::string secret_key = "your-esp32-secret-key-2024";
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 组合数据：MAC:芯片ID:时间戳:密钥
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    
    // SHA256哈希
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    
    // 转换为十六进制字符串（前16字节）
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    
    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 * @param http HTTP客户端指针
 */
static void add_auth_headers(Http* http) {
    // 获取当前时间戳
    int64_t timestamp = esp_timer_get_time() / 1000000;  // 转换为秒
    
    // 生成动态密钥
    std::string dynamic_key = generate_dynamic_key(timestamp);
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 添加认证头
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        
        ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld", 
                 mac.c_str(), chip_id.c_str(), timestamp);
    }
}

// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 在文件开头添加一个辅助函数，统一处理URL构建
static std::string buildUrlWithParams(const std::string& base_url, const std::string& path, const std::string& query) {
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;
    
    while ((amp_pos = query.find("&", pos)) != std::string::npos) {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");
        
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        } else {
            result_url += param + "&";
        }
        
        pos = amp_pos + 1;
    }
    
    // 处理最后一个参数
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");
    
    if (eq_pos != std::string::npos) {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    } else {
        result_url += last_param;
    }
    
    return result_url;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),cover_thread_(),is_cover_running_(false),
                         display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false) {
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    event_group_ = xEventGroupCreate();
    InitializeMp3Decoder();


}

void Esp32Music::InitializeDefaultPlaylists() {
    // 从 NVS 加载已保存的歌单（如果有）
    LoadPlaylistsFromNVS();

    // 开机扫描 SD 中音乐并同步到默认歌单（只在实际变化时写 NVS，避免重复写）
    {
        // 同步扫描（可改为异步以避免启动阻塞）
        if (ScanMusicLibrary("/sdcard/音乐")) {
            // 从 music_library_ 读取文件路径（受 mutex 保护）
            std::vector<std::string> scanned_paths;
            {
                std::lock_guard<std::mutex> lock(music_library_mutex_);
                scanned_paths.reserve(music_library_.size());
                for (const auto& music : music_library_) {
                    scanned_paths.push_back(music.file_path);
                    ESP_LOGI(TAG, "Song: %s, File: %s, Path: %s, Size: %d bytes",
                             music.song_name.c_str(), music.file_name.c_str(), music.file_path.c_str(), music.file_size);
                }
            }

            // 去重并排序，保证比较稳定
            std::sort(scanned_paths.begin(), scanned_paths.end());
            scanned_paths.erase(std::unique(scanned_paths.begin(), scanned_paths.end()), scanned_paths.end());

            int idx = FindPlaylistIndex(this->default_musiclist);
            if (idx == -1) {
                // 默认歌单不存在，只有在扫描到文件时创建并保存
                if (!scanned_paths.empty()) {
                    CreatePlaylist(this->default_musiclist, scanned_paths);
                    SavePlaylistsToNVS();
                    ESP_LOGI(TAG, "Created default playlist from scan with %d songs", (int)scanned_paths.size());
                } else {
                    ESP_LOGI(TAG, "No music found on SD to create default playlist");
                }
            } else {
                // 比较现有歌单与扫描结果，只有不同才替换并保存
                std::vector<std::string> existing;
                {
                    std::lock_guard<std::mutex> lock(music_library_mutex_);
                    existing = playlists_[idx].file_paths;
                }
                //std::sort(...)排序：让相同字符串相邻，为后续去重做准备
                std::sort(existing.begin(), existing.end());
                //std::unique(...)去重：把相邻的重复元素“挤”到尾部，返回新逻辑末尾的迭代器
                //.erase(...)删除：把尾部那坨“重复垃圾”从容器里彻底删掉
                existing.erase(std::unique(existing.begin(), existing.end()), existing.end());

                if (existing != scanned_paths) {
                    {
                        std::lock_guard<std::mutex> lock(music_library_mutex_);
                        playlists_[idx].file_paths = scanned_paths;
                    }
                    SavePlaylistsToNVS();
                    ESP_LOGI(TAG, "Updated default playlist from SD scan (saved to NVS), songs=%d", (int)scanned_paths.size());
                } else {
                    ESP_LOGI(TAG, "Default playlist matches SD scan, skip NVS write");
                }
            }
        } else {
            ESP_LOGW(TAG, "ScanMusicLibrary failed or SD not ready, default playlist not updated");
        }
    }

    LoadPlaybackPosition();

}


Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
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
    
    // 等待歌词线程结束
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }
    
    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());
    
    // 清空之前的下载数据
    last_downloaded_data_.clear();
    
    // 保存歌名用于后续显示
    current_song_name_ = song_name;
    
    // 第一步：请求stream_pcm接口获取音频信息
    std::string base_url = "http://www.xiaozhishop.xyz:5005";
    std::string full_url = base_url + "/stream_pcm?song=" + url_encode(song_name) + "&artist=" + url_encode(artist_name);
    
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    
    // 使用Board提供的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    
    // 添加ESP32认证头
    add_auth_headers(http.get());
    
    // 打开GET连接
    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return false;
    }
    
    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }
    
    // 读取响应数据
    last_downloaded_data_ = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, last_downloaded_data_.length());
    ESP_LOGD(TAG, "Complete music details response: %s", last_downloaded_data_.c_str());
    
    // 简单的认证响应检查（可选）
    if (last_downloaded_data_.find("ESP32动态密钥验证失败") != std::string::npos) {
        ESP_LOGE(TAG, "Authentication failed for song: %s", song_name.c_str());
        return false;
    }
    
    if (!last_downloaded_data_.empty()) {
        // 解析响应JSON以提取音频URL
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        printf("Response JSON: %s\n", last_downloaded_data_.c_str());
        if (response_json) {
            // 提取关键信息
            cJSON* artist = cJSON_GetObjectItem(response_json, "artist");
            cJSON* title = cJSON_GetObjectItem(response_json, "title");
            cJSON* audio_url = cJSON_GetObjectItem(response_json, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(response_json, "lyric_url");
            // cJSON* cover_url = cJSON_GetObjectItem(response_json, "cover_url");
            if (cJSON_IsString(artist)) {
                ESP_LOGI(TAG, "Artist: %s", artist->valuestring);
            }
            if (cJSON_IsString(title)) {
                ESP_LOGI(TAG, "Title: %s", title->valuestring);
                current_song_name_ = title->valuestring;
            }
            
            // 检查audio_url是否有效
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
                ESP_LOGI(TAG, "Audio URL path: %s", audio_url->valuestring);
                
                // 第二步：拼接完整的音频下载URL，确保对audio_url进行URL编码
                std::string audio_path = audio_url->valuestring;
                
                // 使用统一的URL构建功能
                if (audio_path.find("?") != std::string::npos) {
                    size_t query_pos = audio_path.find("?");
                    std::string path = audio_path.substr(0, query_pos);
                    std::string query = audio_path.substr(query_pos + 1);
                    
                    current_music_url_ = buildUrlWithParams(base_url, path, query);
                } else {
                    current_music_url_ = base_url + audio_path;
                }
                
                ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
                ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
                song_name_displayed_ = false;  // 重置歌名显示标志
                StartStreaming(current_music_url_);
                
                // 处理歌词URL - 只有在歌词显示模式下才启动歌词
                if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                    // 拼接完整的歌词下载URL，使用相同的URL构建逻辑
                    std::string lyric_path = lyric_url->valuestring;
                    if (lyric_path.find("?") != std::string::npos) {
                        size_t query_pos = lyric_path.find("?");
                        std::string path = lyric_path.substr(0, query_pos);
                        std::string query = lyric_path.substr(query_pos + 1);
                        
                        current_lyric_url_ = buildUrlWithParams(base_url, path, query);
                    } else {
                        current_lyric_url_ = base_url + lyric_path;
                    }
                    
                    // 根据显示模式决定是否启动歌词
                    if (display_mode_ == DISPLAY_MODE_LYRICS) {
                        ESP_LOGI(TAG, "Loading lyrics for: %s (lyrics display mode)", song_name.c_str());
                        
                        // 启动歌词下载和显示
                        if (is_lyric_running_) {
                            is_lyric_running_ = false;
                            if (lyric_thread_.joinable()) {
                                lyric_thread_.join();
                            }
                        }
                        
                        is_lyric_running_ = true;
                        current_lyric_index_ = -1;
                        lyrics_.clear();
                        
                        lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
                    } else {
                        ESP_LOGI(TAG, "Lyric URL found but spectrum display mode is active, skipping lyrics");
                    }
                } else {
                    ESP_LOGW(TAG, "No lyric URL found for this song");
                }
                
                //  //下载封面
                // if(cJSON_IsString(cover_url) && cover_url->valuestring && strlen(cover_url->valuestring) >0){
                //     ESP_LOGI(TAG, "Cover URL: %s", cover_url->valuestring);
                //     // 拼接完整的封面下载URL，使用相同的URL构建逻辑
                //     std::string cover_path = cover_url->valuestring;
                //     if (cover_path.find("?") != std::string::npos) {
                //         size_t query_pos = cover_path.find("?");
                //         std::string path = cover_path.substr(0, query_pos);
                //         std::string query = cover_path.substr(query_pos + 1);
                        
                //         current_cover_url_ = buildUrlWithParams(base_url, path, query);
                //     } else {
                //         current_cover_url_ = cover_path;
                //     }
                //     // 启动歌词下载和显示
                //     if (is_cover_running_) {
                //         is_cover_running_ = false;
                //         if (cover_thread_.joinable()) {
                //             cover_thread_.join();
                //         }
                //     }
                //         is_cover_running_ = true;
                //         // current_lyric_index_ = -1;
                //         // lyrics_.clear();
                //         cover_thread_ = std::thread(&Esp32Music::CoverDisplayThread, this);
                    
                // } else {
                //     ESP_LOGW(TAG, "No cover URL found for this song");
                // }
                cJSON_Delete(response_json);
                return true;
            } else {
                // audio_url为空或无效
                ESP_LOGE(TAG, "Audio URL not found or empty for song: %s", song_name.c_str());
                ESP_LOGE(TAG, "Failed to find music: 没有找到歌曲 '%s'", song_name.c_str());
                cJSON_Delete(response_json);
                return false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Empty response from music API");
    }
    
    return false;
}



std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    // 停止之前的播放和下载
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
    
    // 清空缓冲区
    ClearAudioBuffer();
    
    // 配置线程栈大小以避免栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;  // 8KB栈大小
    cfg.prio = 5;           // 中等优先级
    cfg.thread_name = "audio_stream";
    esp_pthread_set_cfg(&cfg);
    
    // 开始下载线程
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    
    // 开始播放线程（会等待缓冲区有足够数据）
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    // 重置采样率到原始值
    ResetSampleRate();
    
    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }
    
    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;
    
    // 清空歌名显示
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        // display->SetMusicInfo("");  // 清空歌名显示
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
        const int max_wait = 100; // 最多等待1秒
        
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
    
    // 在线程完全结束后，只在频谱模式下停止FFT显示
    if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
        // display->stopFft();
        ESP_LOGI(TAG, "Stopped FFT display in StopStreaming (spectrum mode)");
    } else if (display) {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop in StopStreaming");
    }
    
    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());
    
    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-");  // 支持断点续传
    
    // 添加ESP32认证头
    add_auth_headers(http.get());
    
    if (!http->Open("GET", music_url)) {
        ESP_LOGE(TAG, "Failed to connect to music stream URL");
        is_downloading_ = false;
        return;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206) {  // 206 for partial content
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        is_downloading_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);
    
    // 分块读取音频数据
    const size_t chunk_size = 4096;  // 4KB每块
    char buffer[chunk_size];
    size_t total_downloaded = 0;
    
    while (is_downloading_ && is_playing_) {
        int bytes_read = http->Read(buffer, chunk_size);
        if (bytes_read < 0) {
            ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            break;
        }
        if (bytes_read == 0) {
            ESP_LOGI(TAG, "Audio stream download completed, total: %d bytes", total_downloaded);
            break;
        }
        
        // 打印数据块信息
        // ESP_LOGI(TAG, "Downloaded chunk: %d bytes at offset %d", bytes_read, total_downloaded);
        
        // 安全地打印数据块的十六进制内容（前16字节）
        if (bytes_read >= 16) {
            // ESP_LOGI(TAG, "Data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X ...", 
            //         (unsigned char)buffer[0], (unsigned char)buffer[1], (unsigned char)buffer[2], (unsigned char)buffer[3],
            //         (unsigned char)buffer[4], (unsigned char)buffer[5], (unsigned char)buffer[6], (unsigned char)buffer[7],
            //         (unsigned char)buffer[8], (unsigned char)buffer[9], (unsigned char)buffer[10], (unsigned char)buffer[11],
            //         (unsigned char)buffer[12], (unsigned char)buffer[13], (unsigned char)buffer[14], (unsigned char)buffer[15]);
        } else {
            ESP_LOGI(TAG, "Data chunk too small: %d bytes", bytes_read);
        }
        
        // 尝试检测文件格式（检查文件头）
        if (total_downloaded == 0 && bytes_read >= 4) {
            if (memcmp(buffer, "ID3", 3) == 0) {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            } else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0) {
                ESP_LOGI(TAG, "Detected MP3 file header");
            } else if (memcmp(buffer, "RIFF", 4) == 0) {
                ESP_LOGI(TAG, "Detected WAV file");
            } else if (memcmp(buffer, "fLaC", 4) == 0) {
                ESP_LOGI(TAG, "Detected FLAC file");
            } else if (memcmp(buffer, "OggS", 4) == 0) {
                ESP_LOGI(TAG, "Detected OGG file");
            } else {
                ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X", 
                        (unsigned char)buffer[0], (unsigned char)buffer[1], 
                        (unsigned char)buffer[2], (unsigned char)buffer[3]);
            }
        }
        
        // 创建音频数据块
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
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
                total_downloaded += bytes_read;
                
                // 通知播放线程有新数据
                buffer_cv_.notify_one();
                
                if (total_downloaded % (256 * 1024) == 0) {  // 每256KB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded, buffer_size_);
                }
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }
    
    http->Close();
    is_downloading_ = false;
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Audio stream download thread finished");
}


bool Esp32Music::WaitForMusicLoaded() {
    ESP_LOGI(TAG, "等待音乐资源准备完毕");

    auto bits = xEventGroupWaitBits(event_group_, MUSIC_EVENT_LOADED, pdTRUE, pdTRUE, portMAX_DELAY);
    return (bits & MUSIC_EVENT_LOADED);
}
// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
        // 新增：周期性保存播放位置
    auto last_pos_save_time = std::chrono::steady_clock::now();
    const int kSaveIntervalMs = 10000 ; // 每10秒保存一次（可按需调整）

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        if(!codec){
            ESP_LOGE(TAG, "Audio codec instance is null");
        } else{
            ESP_LOGE(TAG, "Audio codec output not enabled");
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
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;
    
    Settings settings("music", true); // 可写命名空间 "music"
    settings.SetString("last_playlist", current_playlist_name_);
    const auto& playlist = playlists_[FindPlaylistIndex(current_playlist_name_)];
    settings.SetInt("last_play_index", playlist.play_index);
    
    xEventGroupSetBits(event_group_, MUSIC_EVENT_LOADED);
    while (is_playing_) {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();

        // 等小智把话说完了，变成聆听状态之后，马上转成待机状态，进入音乐播放
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
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                // 格式化歌名显示为《歌名》播放中...
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display) {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
                    // display->start();
                    ESP_LOGI(TAG, "Display start() called for spectrum visualization");
                } else {
                    ESP_LOGI(TAG, "Lyrics display mode active, FFT visualization disabled");
                }
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
                heap_caps_free(chunk.data);
            }
        }
        
        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }
        
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
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;
            
            // 新增：周期性保存播放位置，避免过于频繁写 NVS
            {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pos_save_time).count();
                if (elapsed >= kSaveIntervalMs) {
                    // 异步/快速的保存函数已实现为线程安全且尽量合并写操作
                    SavePlaybackPosition();
                    last_pos_save_time = now;
                }
            }

            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
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
                        mono_buffer[i] = (int16_t)((left + right) / 2);
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
                }
                
                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60;  // 使用Application默认的帧时长
                packet.timestamp = 0;
                
                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                if (final_pcm_data_fft == nullptr) {
                    final_pcm_data_fft = (int16_t*)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM
                    );
                }
                
                memcpy(
                    final_pcm_data_fft,
                    final_pcm_data,
                    final_sample_count * sizeof(int16_t)
                );
                
                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // 发送到Application的音频解码队列
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
                
                // 打印播放进度
                if (total_played % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
            
        } else {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);
            
            // 跳过一些字节继续尝试
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
            } else {
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


    SavePlaybackPosition();
    auto &app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if(state == kDeviceStateIdle){
        
        if(playback_mode_ == PLAYBACK_MODE_ONCE)
            {
                ESP_LOGI(TAG, "Once playback mode active, stopping playback");
                //等待线程自动结束，不用做任何处理
                std::string msg = "再见,你不需要回应"; 
                app.SendMessage(msg);
            }
        else{
                std::string msg = "静默调用工具nextmusic";
                app.SendMessage(msg);
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
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
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

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    // 检查URL是否为空
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }
    
    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // 最多允许5次重定向
    
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }
        
        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        
        // 添加ESP32认证头
        add_auth_headers(http.get());
        
        // 打开GET连接
        ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }
        
        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);
        
        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        
        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");
        
        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
                
                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0) {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            } else if (bytes_read == 0) {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }
        
        http->Close();
        
        if (read_error) {
            retry_count++;
            continue;
        }
        
        // 如果成功读取数据，跳出重试循环
        if (success) {
            break;
        }
    }
    
    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }
    
    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty()) {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content");
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    
    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format) {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        } else {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
}


void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    // display->OnlineMusiclrc_refresh(0,lyrics_);
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    int lrc_top = display->OnlineMusiclrc_get_top();

    // 从当前歌词索引开始查找，提高效率
    //load()是进行一次原子读取并返回当前存储的整数值
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        int new_top = new_lyric_index - 2;
        if (new_top < 0) new_top = 0;
        if (new_top != lrc_top)
            display->lrc_animate_next(new_top);
        
        // auto& board = Board::GetInstance();
        // auto display = board.GetDisplay();
        // if (display) {
        //     std::string lyric_text;
            
        //     if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
        //         lyric_text = lyrics_[current_lyric_index_].second;
        //     }
            
        //     // 显示歌词
            // display->SetChatMessage("lyric", lyric_text.c_str());
            
        //     ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
        //             current_time_ms, 
        //             lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        // }
    }
}


bool Esp32Music::DownloadCover(const std::string& cover_url)
{
    ESP_LOGI(TAG, "Downloading cover from: %s", cover_url.c_str());
    
    // 检查URL是否为空
    if (cover_url.empty()) {
        ESP_LOGE(TAG, "Cover URL is empty!");
        return false;
    }

    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string cover_content;
    std::string current_url = cover_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // 最多允许5次重定向
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if(retry_count > 0) {
            ESP_LOGI(TAG, "Retrying cover download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for cover download");
            retry_count++;
            continue;   
        }
        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        // http->SetHeader("Accept", "");
        // http->SetHeader("Accept", "image/jpeg,image/png,*/*;q=0.8");
        add_auth_headers(http.get());
        // 打开GET连接
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for cover");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }

        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Cover download HTTP status code: %d", status_code);
        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        // 读取响应
        cover_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGI(TAG, "Starting to read cover content");
        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Cover HTTP read returned %d bytes", bytes_read);  // 注释掉以减少日志输出
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                cover_content += buffer;
                total_read += bytes_read;
                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0) {
                    ESP_LOGI(TAG, "Downloaded %d bytes so far", total_read);
                }
            } else if (bytes_read == 0) {
                // 正常结束，没有更多数据
                ESP_LOGI(TAG, "Cover download completed, total bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!cover_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, cover_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read cover data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
    }
        http->Close();
        if (read_error) {
            retry_count++;
            continue;
        }
        // 如果成功读取数据，跳出重试循环
        if (success) {
            break;
        }
    }
    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download cover after %d attempts", max_retries);
        return false;
    }
    if(!cover_content.empty()) {
        size_t preview_size = std::min(cover_content.size(), size_t(50));
        std::string preview = cover_content.substr(0, preview_size);
        // ESP_LOGI(TAG, "Cover content preview (%d bytes): %s", cover_content.length(), preview.c_str());
        ESP_LOG_BUFFER_HEX("RAW", cover_content.data(),
                   std::min(64, (int)cover_content.length()));
    } else {
        ESP_LOGE(TAG, "Failed to download cover or cover is empty");
        return false;
    }
    ESP_LOGI(TAG, "Cover downloaded successfully, size: %d bytes", cover_content.length());
    // cover_content_ = std::move(cover_content);
    return ParseCover(cover_content);
}

static uint8_t *cover_buf = nullptr;   // 存上一张图
esp_err_t process_jpeg(uint8_t *jpeg_data, size_t jpeg_size, uint8_t **rgb565_data, size_t *rgb565_size, size_t *width, size_t *height);
extern lv_obj_t *img_cover;          /* 封面 */
bool Esp32Music::ParseCover(const std::string& cover_content) {
    ESP_LOGI(TAG, "Parsing cover content");
    uint8_t *rgb565_data = NULL;
    size_t rgb565_size = 0;
    size_t width = 0, height = 0;
    static lv_img_dsc_t img_dsc_cover_ = {0};
    if(!process_jpeg((uint8_t *)cover_content.data(), cover_content.length(), &rgb565_data, &rgb565_size, &width, &height))
    {
        ESP_LOGE(TAG, "Failed to process JPEG cover image");
        return false;
    }
    if(cover_buf)
    {
        free(cover_buf);
        cover_buf = NULL;
    }
    cover_buf = rgb565_data; // 保存上一张图的buf，下一次释放
    // 设置LVGL图像描述符
    img_dsc_cover_.data = cover_buf;
    img_dsc_cover_.data_size = rgb565_size;
    img_dsc_cover_.header.w = width;
    img_dsc_cover_.header.h = height;
    img_dsc_cover_.header.cf = LV_COLOR_FORMAT_RGB565;
    lv_image_set_src(img_cover, &img_dsc_cover_);  // 使用下载的封面
    return true;
}


void Esp32Music::CoverDisplayThread() {
    ESP_LOGI(TAG, "Cover display thread started");
    
    if (!DownloadCover(current_cover_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse Cover");
        is_cover_running_ = false;
        return;
    }
    
    // // 定期检查是否需要更新显示(频率可以降低)
    // while (is_cover_running_ && is_playing_) {
    //     std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // }
    
    ESP_LOGI(TAG, "Cover display thread finished");
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

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode) {
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;
    
    ESP_LOGI(TAG, "Display mode changed from %s to %s", 
            (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
            (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");
}



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


// 规范化歌单名称：去除首尾空白并去掉尾部的 "的歌单"
static std::string NormalizePlaylistName(std::string name) {
    // trim
    while (!name.empty() && isspace((unsigned char)name.front())) name.erase(name.begin());
    while (!name.empty() && isspace((unsigned char)name.back())) name.pop_back();

    const std::string suffix = "的歌单";
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
        // trim again in case there is trailing space before suffix
        while (!name.empty() && isspace((unsigned char)name.back())) name.pop_back();
    }
    return name;
}

// ========== 新增的SD卡播放功能 ==========


void Esp32Music::SetLoopMode(bool loop) {
    playback_mode_ = loop ? PLAYBACK_MODE_LOOP : PLAYBACK_MODE_ONCE;
}
void Esp32Music::SetRandomMode(bool random) {
    playback_mode_ = random ? PLAYBACK_MODE_RANDOM : PLAYBACK_MODE_ONCE;
}
void Esp32Music::SetOnceMode(bool once) {
    playback_mode_ = once ? PLAYBACK_MODE_ONCE : PLAYBACK_MODE_ONCE;
}
void Esp32Music::SetOrderMode(bool order) {
    playback_mode_ = order ? PLAYBACK_MODE_ORDER : PLAYBACK_MODE_ONCE;
}

PlaybackMode Esp32Music::GetPlaybackMode() {
    return playback_mode_;
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
    current_song_name_ = song_name.empty() ? file_path : song_name;
    
    // 停止之前的播放
    StopStreaming();
    
    // 清空之前的下载数据
    last_downloaded_data_.clear();
    
    // 开始SD卡流式播放
    song_name_displayed_ = false;
    return StartSDCardStreaming(file_path);
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
    
    // // 记录当前播放文件路径（保护并发访问），保证 SavePlaybackPosition 能回退到有效路径
    // {
    //     std::lock_guard<std::mutex> lock(music_library_mutex_);
    //     current_play_file_path_ = file_path;
    // }

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
    
    // 清空缓冲区
    ClearAudioBuffer();
    
    // 配置线程栈大小
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;
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
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        if (start_play_offset_ > 0) {
            if (fseek(file, (long)start_play_offset_, SEEK_SET) == 0) {
                current_play_file_offset_ = start_play_offset_;
                ESP_LOGI(TAG, "Seeked SD file %s to offset %llu", file_path.c_str(), (unsigned long long)start_play_offset_);
            } else {
                ESP_LOGW(TAG, "Failed to seek SD file to offset %llu, starting at 0", (unsigned long long)start_play_offset_);
                current_play_file_offset_ = 0;
            }
            // 使用完毕清零（只针对本次播放）
            start_play_offset_ = 0;
        } else {
            current_play_file_offset_ = 0;
        }

        // 保存当前文件指针以供其他代码读取/保存偏移
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
    
    while (is_downloading_ && is_playing_) {
        size_t bytes_read = fread(buffer, 1, chunk_size, file);
        if (bytes_read == 0) {
            if (feof(file)) {
                ESP_LOGI(TAG, "SD card file read completed, total: %d bytes", total_read);
            } else {
                ESP_LOGE(TAG, "Failed to read from file");
            }
            break;
        }
        
        // 更新当前播放文件偏移
        {
            std::lock_guard<std::mutex> lock(current_play_file_mutex_);
            current_play_file_offset_ += bytes_read;
        }

        // 创建音频数据块
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
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
                heap_caps_free(chunk_data);
                break;
            }
        }
    }
    
    heap_caps_free(buffer);
    fclose(file);

    // 清理 current_play_file_ 和偏移（保留偏移用于最后一次保存）
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        current_play_file_ = nullptr;
        // current_play_file_offset_ 保留为已读字节数，SavePlaybackPosition 会读取并保存
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

bool Esp32Music::ScanMusicLibrary(const std::string& music_folder) {
    ESP_LOGI(TAG, "Scanning music library from: %s", music_folder.c_str());
    
    // 检查目录是否存在
    if (!file_exists(music_folder)) {
        ESP_LOGE(TAG, "Music folder does not exist: %s", music_folder.c_str());
        return false;
    }
    
    if (!is_directory(music_folder)) {
        ESP_LOGE(TAG, "Path is not a directory: %s", music_folder.c_str());
        return false;
    }
    
    // 清空现有音乐库
    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        music_library_.clear();
        music_library_scanned_ = false;
    }
    
    // 递归扫描目录
    ScanDirectoryRecursive(music_folder);
    
    // 更新扫描状态
    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        music_library_scanned_ = true;
    }
    
    ESP_LOGI(TAG, "Music library scan completed, found %d music files", music_library_.size());
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
        // 跳过 "." 和 ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        std::string full_path = path + "/" + entry->d_name;
        
        //如果是目录，递归扫描
        if (entry->d_type == DT_DIR) {
            // 递归扫描子目录
            ESP_LOGD(TAG, "Scanning subdirectory: %s", full_path.c_str());
            ScanDirectoryRecursive(full_path);
            dir_count++;
        } 
        //如果是文件，检查是否是音乐文件
        else if (entry->d_type == DT_REG) {
            // 检查是否是音乐文件
            if (IsMusicFile(full_path)) {
                MusicFileInfo music_info = ExtractMusicInfo(full_path);
                
                std::lock_guard<std::mutex> lock(music_library_mutex_);
                music_library_.push_back(music_info);
                
                file_count++;
                
                // 每扫描10个文件打印一次进度
                if (file_count % 10 == 0) {
                    ESP_LOGI(TAG, "Scanned %d music files...", file_count);
                }
                
                ESP_LOGD(TAG, "Found music file: %s (%d bytes)", 
                        music_info.file_name.c_str(), music_info.file_size);
            }
        }
    }
    
    closedir(dir);
    
    ESP_LOGD(TAG, "Scanned directory %s: %d files, %d subdirectories", 
             path.c_str(), file_count, dir_count);
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

    // 从文件路径提取文件名
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
    ESP_LOGI(TAG, "Extracted music info - File: %s, Artist: %s, Song: %s", 
            info.file_name.c_str(), info.artist.c_str(), info.song_name.c_str());
    // 获取文件大小
    info.file_size = get_file_size(file_path);

    return info;
}


std::string NormalizeForSearch(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            // ASCII：保留字母数字（字母转小写），丢弃空白与标点
            if (std::isalnum(c)) {
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

// 解析文件名/输入，返回歌手和歌名以及它们的规范化形式
SongMeta ParseSongMeta(const std::string& filename) {
    SongMeta meta;
    std::string name = filename;

    // 去掉路径部分
    size_t pos = name.find_last_of("/\\");
    if (pos != std::string::npos) name = name.substr(pos + 1);

    // 去掉扩展名
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);

    // 初始化原始 title 为整个文件名（后面可能被替换）
    meta.title = name;
    meta.artist.clear();

    // 如果包含 '-'，按第一个 '-' 分割为 artist - title
    size_t dash = name.find('-');
    if (dash != std::string::npos) {
        std::string left = name.substr(0, dash);
        std::string right = name.substr(dash + 1);

        // trim 左右空白
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
            // trim right
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

std::string Esp32Music::ExtractSongNameFromFileName(const std::string& file_name) const {
    std::string song_name = file_name;
    std::string singer_name;
    // 去掉路径部分
    size_t pos = song_name.find_last_of("/\\");
    if (pos != std::string::npos) song_name = song_name.substr(pos + 1);

    // 去掉扩展名（如 .mp3 .wav 等）
    size_t dot = song_name.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = song_name.substr(dot);
        const char* exts[] = {".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg", ".wma", nullptr};
        bool known = false;
        for (int i = 0; exts[i] != nullptr; ++i) {
            if (ext == exts[i]) { known = true; break; }
        }
        if (known) song_name.erase(dot);
    }

    // 如果文件名是 "Artist - Song" 或 "Artist-Song" 格式，取 '-' 后面的部分作为曲名
    size_t dash = song_name.find('-');
    if (dash != std::string::npos) {
        std::string left = song_name.substr(0, dash);
        std::string right = song_name.substr(dash + 1);
        //检查 left 中是否至少有一个字母字符，并且 right 非空且 left 不太长
        bool left_has_alpha = std::any_of(left.begin(), left.end(), [](unsigned char c){ return std::isalpha(c); });
        bool right_nonempty = std::any_of(right.begin(), right.end(), [](unsigned char c){ return !std::isspace(c); });
        if (left_has_alpha && right_nonempty && left.size() < 128) {
            song_name = right;
            singer_name = left;
        }
    }

    // // 去掉常见后缀（如 "(Official Video)" 等）
    // const char* suffixes[] = {
    //     " (official video)", " (official audio)", " (audio)", " (lyrics)",
    //     " [official audio]", " [lyrics]", " - official", nullptr
    // };
    // std::string tmp = song_name;
    // std::transform(tmp.begin(), tmp.end(), tmp.begin(), [](unsigned char c){ return std::tolower(c); });
    // for (int i = 0; suffixes[i] != nullptr; ++i) {
    //     auto s = std::string(suffixes[i]);
    //     size_t p = tmp.find(s);
    //     if (p != std::string::npos) {
    //         song_name.erase(p);
    //         tmp.erase(p);
    //     }
    // }

    // 去除首尾空白
    size_t start = song_name.find_first_not_of(" \t\r\n");
    size_t end = song_name.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    song_name = song_name.substr(start, end - start + 1);

    // 规范化：转小写并移除所有非字母数字字符（包含空格和特殊字符）
    std::transform(song_name.begin(), song_name.end(), song_name.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    song_name.erase(std::remove_if(song_name.begin(), song_name.end(),
                                   [](unsigned char c){ return !std::isalnum(c); }),
                    song_name.end());

    return song_name;
}

MusicFileInfo Esp32Music::GetMusicInfo(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    
    for (const auto& music_info : music_library_) {
        if (music_info.file_path == file_path) {
            return music_info;
        }
    }
    
    // 如果没有找到，返回一个空的MusicFileInfo
    return MusicFileInfo();
}


std::vector<MusicFileInfo> Esp32Music::GetMusicLibrary() const {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    return music_library_;
}



// ========== 播放列表功能实现 ==========

int Esp32Music::FindPlaylistIndex(const std::string& name) const {

    for (size_t i = 0; i < playlists_.size(); i++) {
        if (playlists_[i].name == name) {
            return i;
        }
    }
    return -1;
}

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
    
    // 检查播放列表是否已存在
    if (FindPlaylistIndex(playlist_name) != -1) {
        ESP_LOGE(TAG, "Playlist already exists: %s", playlist_name.c_str());
        return false;
    }
    // 创建新播放列表
    Playlist new_playlist(playlist_name);
    new_playlist.file_paths = file_paths;
    playlists_.push_back(new_playlist);

    ESP_LOGI(TAG, "Created playlist '%s' with %d songs", playlist_name.c_str(), file_paths.size());
    return true;
}

bool Esp32Music::CreatePlaylist(const std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    
    // 检查播放列表是否已存在
    if (FindPlaylistIndex(playlist_name) != -1) {
        ESP_LOGE(TAG, "Playlist already exists: %s", playlist_name.c_str());
        return false;
    }
    
    // 创建新播放列表
    Playlist new_playlist(playlist_name);
    
    playlists_.push_back(new_playlist);
    
    ESP_LOGI(TAG, "Created empty playlist '%s'", playlist_name.c_str());
    return true;
}
void Esp32Music::SetPlayIndex(std::string& playlist_name, int index) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    int list_index = FindPlaylistIndex(playlist_name);
    if (list_index == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return;
    }
    auto& playlist = playlists_[list_index];
    playlist.last_play_index = playlist.play_index;
    playlist.play_index = index;
    if(playlist.play_index >= playlist.file_paths.size())
        playlist.play_index = playlist.file_paths.size();
}

void Esp32Music::NextPlayIndexOrder(std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    int list_index = FindPlaylistIndex(playlist_name);
    if (list_index == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return;
    }
    auto& playlist = playlists_[list_index];
    playlist.last_play_index = playlist.play_index;
    playlist.play_index++;
    if(playlist.play_index >= playlist.file_paths.size())
        playlist.play_index = 0;
    ESP_LOGI(TAG, "Order next play index: %d", playlist.play_index);
}

int Esp32Music::GetLastPlayIndex(std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    int list_index = FindPlaylistIndex(playlist_name);
    if (list_index == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return -1;
    }
    auto& playlist = playlists_[list_index];
    return playlist.last_play_index;
}

std::string Esp32Music::GetCurrentPlayList(void) {

    return current_playlist_name_;
}
void Esp32Music::NextPlayIndexRandom(std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    int list_index = FindPlaylistIndex(playlist_name);
    if (list_index == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return;
    }
    auto& playlist = playlists_[list_index];
    playlist.last_play_index = playlist.play_index;
    // 确保随机索引不同于当前索引
    do {
        playlist.play_index = esp_random() % playlist.file_paths.size();
    } while (playlist.play_index == playlist.last_play_index);
    ESP_LOGI(TAG, "Random next play index: %d", playlist.play_index);
}


void Esp32Music::SetCurrentPlayList(const std::string& playlist_name) {
    current_playlist_name_ = playlist_name;
}

bool Esp32Music::PlayPlaylist(std::string& playlist_name) {
    std::lock_guard<std::mutex> lock(music_library_mutex_);

    int list_index = FindPlaylistIndex(playlist_name);
    if (list_index == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return false;
    }
    
    const auto& playlist = playlists_[list_index];
    if (playlist.file_paths.empty()) {
        ESP_LOGE(TAG, "Playlist is empty: %s", playlist_name.c_str());
        return false;
    }
    current_playlist_name_ = playlist_name;
    bool result = PlayFromSD(playlist.file_paths[playlist.play_index]);
    return result;
}

std::vector<std::string> Esp32Music::GetPlaylistNames() const {
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    
    for (const auto& playlist : playlists_) {
        names.push_back(playlist.name);
    }
    return names;
}

std::vector<MusicFileInfo> Esp32Music::GetPlaylist(const std::string& playlist_name) const {
    std::vector<MusicFileInfo> playlist_info;
    std::lock_guard<std::mutex> lock(music_library_mutex_);
    
    int index = FindPlaylistIndex(playlist_name);
    if (index == -1) {
        return playlist_info;
    }
    
    const auto& playlist = playlists_[index];
    for (const auto& file_path : playlist.file_paths) {
        MusicFileInfo info = GetMusicInfo(file_path);
        if (!info.file_path.empty()) {
            playlist_info.push_back(info);
        }
    }
    
    return playlist_info;
}

// 在播放列表中按规范化键搜索（支持 Artist-Song 的组合匹配）
int Esp32Music::SearchMusicIndexFromlist(std::string name, const std::string& playlist_name) const {
    int pindex = FindPlaylistIndex(playlist_name);
    if (pindex == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return -1;
    }

    // 规范化输入
    std::string target = NormalizeForSearch(name);

    const auto& playlist = playlists_[pindex];
    for (size_t i = 0; i < playlist.file_paths.size(); ++i) {
        MusicFileInfo info = GetMusicInfo(playlist.file_paths[i]);
        if (info.file_path.empty()) continue;

        // info.song_name / info.artist_norm 已由 ExtractMusicInfo 填充并规范化
        std::string song_key = info.song_name;
        std::string artist_key = info.artist_norm;
        std::string combined_key = artist_key.empty() ? song_key : (artist_key + song_key);

        if (song_key == target || combined_key == target) {
            ESP_LOGI(TAG, "Found song '%s' in playlist '%s' at index %d", name.c_str(), playlist_name.c_str(), (int)i);
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 在播放列表中按规范化键搜索（支持 Artist-Song 的组合匹配）
std::vector<std::string> Esp32Music::SearchSingerFromlist(std::string singer, const std::string& playlist_name) const {
    int pindex = FindPlaylistIndex(playlist_name);
    if (pindex == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return {};
    }

    // 规范化输入
    std::string target = NormalizeForSearch(singer);
    std::vector<std::string> results;
    const auto& playlist = playlists_[pindex];
    for (size_t i = 0; i < playlist.file_paths.size(); ++i) {
        MusicFileInfo info = GetMusicInfo(playlist.file_paths[i]);
        if (info.file_path.empty()) continue;

        std::string artist_key = info.artist_norm;
        if (artist_key == target) {
            ESP_LOGI(TAG, "Found singer '%s' in playlist '%s' at index %d", singer.c_str(), playlist_name.c_str(), (int)i);
            results.push_back(info.song_name);
        }
    }
    return results;
}

std::string Esp32Music::SearchMusicPathFromlist(std::string name, const std::string& playlist_name) const {
    int pindex = FindPlaylistIndex(playlist_name);
    if (pindex == -1) {
        ESP_LOGE(TAG, "Playlist not found: %s", playlist_name.c_str());
        return "";
    }

    std::string target = NormalizeForSearch(name);
    const auto& playlist = playlists_[pindex];
    for (size_t i = 0; i < playlist.file_paths.size(); ++i) {
        MusicFileInfo info = GetMusicInfo(playlist.file_paths[i]);
        if (info.file_path.empty()) continue;

        std::string song_key = info.song_name;
        std::string artist_key = info.artist_norm;
        std::string combined_key = artist_key.empty() ? song_key : (artist_key + song_key);

        if (song_key == target || combined_key == target) {
            ESP_LOGI(TAG, "Found song '%s' in playlist '%s' at index %d", name.c_str(), playlist_name.c_str(), (int)i);
            return info.file_path;
        }
    }
    return "";
}

void Esp32Music::AddMusicToDefaultPlaylists(std::vector<std::string> default_music_files) {
    ESP_LOGI(TAG, "Adding music to default playlists");
    CreatePlaylist(default_musiclist, default_music_files);
    SavePlaylistsToNVS();
}


std::string Esp32Music::GetCurrentSongName()
{
    return current_song_name_;
}

// 异步保存控制标志（文件范围静态，避免修改头文件）
static std::atomic<bool> save_in_progress(false);
static std::atomic<bool> save_pending(false);
void Esp32Music::SavePlaylistsToNVS() {
    // 复制当前 playlists_ 快照（锁内）
    std::vector<Playlist> snapshot;
    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        snapshot = playlists_;
    }

    // 如果已有保存进行中，只标记 pending 并返回（后台线程会处理后续变更）
    if (save_in_progress.exchange(true)) {
        save_pending.store(true);
        return;
    }

    // 启动后台写线程（分离）
    std::thread([snapshot,this]() mutable {
        while (true) {
            // 序列化 snapshot 到 JSON
            cJSON* root = cJSON_CreateArray();
            for (const auto& pl : snapshot) {
                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", pl.name.c_str());
                ESP_LOGI(TAG, "Serializing playlist '%s' ", pl.name.c_str());
                cJSON* files = cJSON_CreateArray();
                for (const auto& f : pl.file_paths) {
                    cJSON_AddItemToArray(files, cJSON_CreateString(f.c_str()));
                }
                cJSON_AddItemToObject(obj, "files", files);
                cJSON_AddItemToArray(root, obj);
            }
            char* json_str = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            if (json_str) {
                // 写入 NVS（使用 Settings，会在析构时 commit）
                Settings settings("music", true);
                settings.SetString("playlists", json_str);
                free(json_str);
                ESP_LOGI(TAG, "Saved %d playlists to NVS (async)", (int)snapshot.size());
            } else {
                ESP_LOGW(TAG, "Failed to serialize playlists to JSON (async)");
            }

            // 写入完成，检查是否有 pending（在此后台运行期间外部调用了 SavePlaylistsToNVS）
            if (save_pending.exchange(false)) {
                // 重新获取最新 snapshot

                    std::lock_guard<std::mutex> lock(this->music_library_mutex_);
                    snapshot = this->playlists_;
                    continue; // 继续循环，执行下一次保存
            }

            break; // 无 pending，完成所有保存
        }

        // 释放 in_progress 标志
        save_in_progress.store(false);
    }).detach();
}




bool Esp32Music::DeletePlaylistFromNVS(const std::string& playlist_name) {
    if (playlist_name.empty()) {
        ESP_LOGE(TAG, "DeletePlaylistFromNVS called with empty name");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        int idx = FindPlaylistIndex(playlist_name);
        if (idx == -1) {
            ESP_LOGW(TAG, "DeletePlaylistFromNVS: playlist not found: %s", playlist_name.c_str());
            return false;
        }

        // 如果正在播放该歌单，先停止播放（防止删除后引用失效）
        bool was_current = (current_playlist_name_ == playlist_name);
        if (was_current) {
            ESP_LOGI(TAG, "Deleting current playlist '%s' - stopping playback", playlist_name.c_str());
            // 设置停止标志并通知线程安全退出
            is_playing_ = false;
            is_downloading_ = false;
            {
                std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            // 清空当前播放歌单指示，避免后续保存写入已删除歌单
            current_playlist_name_.clear();
        }

        // 从内存中移除歌单
        playlists_.erase(playlists_.begin() + idx);
        ESP_LOGI(TAG, "Deleted playlist '%s' from memory", playlist_name.c_str());
    }

    // 持久化变更（异步写入）
    SavePlaylistsToNVS();

    // 如果 NVS 中保存的断点指向此歌单，则清除相关断点信息
    Settings settings("music", true);
    std::string saved_pl = settings.GetString("last_playlist", "");
    if (saved_pl == playlist_name) {
        settings.EraseKey("last_playlist");
        settings.EraseKey("last_play_index");
        settings.EraseKey("last_play_ms");
        settings.EraseKey("lastfileoffset");
        settings.EraseKey("last_music_name");
        settings.Commit();
        // 更新内存状态
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        has_saved_position_ = false;
        saved_playlist_name_.clear();
        saved_play_index_ = -1;
        saved_play_ms_ = 0;
        saved_file_offset_ = 0;
        // saved_file_path_.clear();
        ESP_LOGI(TAG, "Cleared saved playback position referencing deleted playlist '%s' from NVS", playlist_name.c_str());
    }

    return true;
}


bool Esp32Music::DeleteAllPlaylistsExceptDefault() {
    // 先在锁内删除内存中的歌单（记录是否需要停止播放某个正在播放的歌单）
    std::string playing_to_stop;
    bool removed_any = false;
    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        for (auto it = playlists_.begin(); it != playlists_.end(); ) {
            if (it->name != default_musiclist) {
                // 如果要删除的是当前正在播放的歌单，记下来稍后停止播放（避免在持锁时调用可能阻塞的 StopStreaming）
                if (!current_playlist_name_.empty() && current_playlist_name_ == it->name) {
                    playing_to_stop = it->name;
                    current_playlist_name_.clear(); // 先清除当前标识，避免其它线程再使用
                }
                it = playlists_.erase(it);
                removed_any = true;
            } else {
                ++it;
            }
        }
    }

    // 如果需要停止播放，脱锁后再安全停止播放线程
    if (!playing_to_stop.empty()) {
        ESP_LOGI(TAG, "Deleted current playlist '%s', stopping playback", playing_to_stop.c_str());
        // StopStreaming 可能阻塞并需要其他锁；异步或同步调用视你的 StopStreaming 实现而定
        // StopStreaming();
    }

    // // 持久化歌单变更
    // SavePlaylistsToNVS();

    // // 清理 NVS 中引用已删除歌单的断点记录（如果 last_playlist 不再存在）
    // Settings settings("music", true);
    // std::string saved_pl = settings.GetString("last_playlist", "");
    // if (!saved_pl.empty() && saved_pl != default_musiclist) {
    //     // 如果 saved_pl 不在内存歌单中，则清除断点相关键
    //     bool found = false;
    //     {
    //         std::lock_guard<std::mutex> lock(music_library_mutex_);
    //         for (const auto& pl : playlists_) {
    //             if (pl.name == saved_pl) { found = true; break; }
    //         }
    //     }
    //     if (!found) {
    //         settings.EraseKey("last_playlist");
    //         settings.EraseKey("last_play_index");
    //         settings.EraseKey("last_play_ms");
    //         settings.EraseKey("last_file_path");
    //         settings.EraseKey("last_file_offset");
    //         settings.EraseKey("last_music_name");
    //         settings.Commit();
    //         // 更新内存状态
    //         std::lock_guard<std::mutex> lock(music_library_mutex_);
    //         has_saved_position_ = false;
    //         saved_playlist_name_.clear();
    //         saved_play_index_ = -1;
    //         saved_play_ms_ = 0;
    //         saved_file_offset_ = 0;
    //         saved_file_path_.clear();
    //         ESP_LOGI(TAG, "Cleared saved playback position referencing deleted playlists");
    //     }
    // }

    ESP_LOGI(TAG, "DeleteAllPlaylistsExceptDefault completed, removed_any=%d", removed_any ? 1 : 0);
    return removed_any;
}

// 从 NVS 加载 playlists_
bool Esp32Music::LoadPlaylistsFromNVS() {
    Settings settings("music", false);
    std::string json = settings.GetString("playlists", "");
    if (json.empty()) {
        ESP_LOGI(TAG, "No playlists found in NVS");
        return false;
    }
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "Invalid playlists JSON in NVS");
        if (root) cJSON_Delete(root);
        return false;
    }

    std::lock_guard<std::mutex> lock(music_library_mutex_);
    playlists_.clear();
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* files = cJSON_GetObjectItem(item, "files");
        if (!cJSON_IsString(name) || !cJSON_IsArray(files)) continue;
        Playlist pl(name->valuestring);
        cJSON* f = nullptr;
        cJSON_ArrayForEach(f, files) {
            if (cJSON_IsString(f)) pl.file_paths.push_back(f->valuestring);
        }
        playlists_.push_back(std::move(pl));
        ESP_LOGI(TAG, "Loaded playlist '%s' ", pl.name.c_str());
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d playlists from NVS", (int)playlists_.size());
    return true;
}




void Esp32Music::LoadPlaybackPosition() {
    Settings settings("music", false);
    std::string plist = settings.GetString("last_playlist", "");
    if (plist.empty()) {
        has_saved_position_ = false;
        ESP_LOGI(TAG, "No saved playback position in NVS");
        return;
    }
    int idx = settings.GetInt("last_play_index", -1);
    int32_t ms = settings.GetInt("last_play_ms", 0);
    int64_t offset_i64 = settings.GetInt64("lastfileoffset", 0);
    auto music_name = settings.GetString("last_music_name", "");
    // std::string saved_path = settings.GetString("last_file_path", "");
    size_t offset = offset_i64 > 0 ? static_cast<size_t>(offset_i64) : 0;

    saved_playlist_name_ = plist;
    saved_play_index_ = idx;
    saved_play_ms_ = ms;
    saved_file_offset_ = offset;
    // saved_file_path_ = saved_path;
    has_saved_position_ = true;
    current_playlist_name_ = saved_playlist_name_;
    current_song_name_ = music_name;

    // // 额外：如果保存了 file_path，则尽量在对应歌单中根据路径寻找正确的索引（防止索引重排）
    // if (!saved_file_path_.empty()) {
    //     int pidx = FindPlaylistIndex(saved_playlist_name_);
    //     if (pidx != -1) {
    //         const auto &files = playlists_[pidx].file_paths;
    //         int found = -1;
    //         for (size_t i = 0; i < files.size(); ++i) {
    //             if (files[i] == saved_file_path_) { found = (int)i; break; }
    //         }
    //         if (found != -1) {
    //             saved_play_index_ = found;
    //             ESP_LOGI(TAG, "Recovered saved file path in playlist '%s' -> index %d", saved_playlist_name_.c_str(), saved_play_index_);
    //         } else {
    //             ESP_LOGW(TAG, "Saved file path not found in playlist '%s', will try saved index %d", saved_playlist_name_.c_str(), saved_play_index_);
    //         }
    //     } else {
    //         ESP_LOGW(TAG, "Saved playlist '%s' not found when resolving saved file path", saved_playlist_name_.c_str());
    //     }
    //         // ESP_LOGI(TAG, "Loaded saved playback pos: playlist='%s' index=%d ms=%d offset=%llu path='%s'",
    //         //  saved_playlist_name_.c_str(), saved_play_index_, (int)saved_play_ms_, (unsigned long long)saved_file_offset_,
    //         //  saved_file_path_.c_str());

    // }
    // else {
        ESP_LOGI(TAG, "Loaded saved playback pos: playlist='%s' index=%d ms=%d offset=%llu",
             saved_playlist_name_.c_str(), saved_play_index_, (int)saved_play_ms_, (unsigned long long)saved_file_offset_
             );
    // }

}


void Esp32Music::SavePlaybackPosition() {
    // 先读取需要保存的状态（在 mutex 内确保一致性）
    std::string playlist_copy;
    int play_index = -1;
    std::string path_from_playlist;
    {
        std::lock_guard<std::mutex> lock(music_library_mutex_);
        playlist_copy = current_playlist_name_;
        if (!playlist_copy.empty()) {
            int idx = FindPlaylistIndex(playlist_copy);
            if (idx != -1) {
                // 先把 play_index 读取到本地并校验范围
                play_index = playlists_[idx].play_index;
                if (play_index >= 0 && play_index < (int)playlists_[idx].file_paths.size()) {
                    path_from_playlist = playlists_[idx].file_paths[play_index];
                } else {
                    ESP_LOGW(TAG, "SavePlaybackPosition: play_index %d out of range for playlist '%s' (size=%u)",
                             play_index, playlist_copy.c_str(), (unsigned)playlists_[idx].file_paths.size());
                }
            } else {
                ESP_LOGW(TAG, "SavePlaybackPosition: current playlist '%s' not found", playlist_copy.c_str());
            }
        } else {
            // nothing to save
            ESP_LOGD(TAG, "SavePlaybackPosition: no current playlist, skip saving");
            return;
        }
    }

    // 读取 current_play_file_offset_
    size_t file_offset = 0;
    {
        std::lock_guard<std::mutex> lock(current_play_file_mutex_);
        file_offset = current_play_file_offset_;
    }

    // // 若 playlist 路径为空则回退到 current_play_file_path_
    // std::string final_path;
    // {
    //     std::lock_guard<std::mutex> lock(music_library_mutex_);
    //     final_path = path_from_playlist.empty() ? current_play_file_path_ : path_from_playlist;
    // }

    int64_t play_ms = current_play_time_ms_;

    Settings settings("music", true);
    settings.SetString("last_playlist", playlist_copy);
    settings.SetInt("last_play_index", play_index);
    settings.SetInt("last_play_ms", static_cast<int32_t>(play_ms));
    settings.SetInt64("lastfileoffset", (int64_t)file_offset);
    settings.SetString("last_music_name", current_song_name_);
    // if (!final_path.empty()) {
    //     settings.SetString("last_file_path", final_path);
    // } else {
    //     // 如果最终路径也为空，仍然写入但记录警告
    //     ESP_LOGW(TAG, "SavePlaybackPosition: final_path empty for playlist='%s' index=%d", playlist_copy.c_str(), play_index);
    // }
    settings.Commit();


    ESP_LOGI(TAG, "Saved playback pos: playlist='%s' index=%d ms=%lld offset=%llu ",
             playlist_copy, play_index, (long long)play_ms, (unsigned long long)file_offset);
}

bool Esp32Music::ResumeSavedPlayback() {
    if (!has_saved_position_) {
        ESP_LOGI(TAG, "No saved playback position to resume");
        return false;
    }

    // 确保歌单已加载
    int pidx = FindPlaylistIndex(saved_playlist_name_);
    if (pidx == -1) {
        ESP_LOGW(TAG, "Saved playlist not found: %s", saved_playlist_name_.c_str());
        return false;
    }

    // // 优先按文件路径恢复（如果存在）
    std::string file_path;
    // if (!saved_file_path_.empty()) {
    //     // 确认路径在歌单里
    //     const auto &files = playlists_[pidx].file_paths;
    //     auto it = std::find(files.begin(), files.end(), saved_file_path_);
    //     if (it != files.end()) {
    //         file_path = *it;
    //         saved_play_index_ = (int)std::distance(files.begin(), it);
    //         ESP_LOGI(TAG, "Resuming by saved file path: %s (index %d)", file_path.c_str(), saved_play_index_);
    //     } else {
    //         ESP_LOGW(TAG, "Saved file path not found in playlist, falling back to saved index");
    //     }
    // }

    if (saved_play_index_ < 0 || saved_play_index_ >= (int)playlists_[pidx].file_paths.size()) {
        ESP_LOGW(TAG, "Saved play index out of range: %d", saved_play_index_);
        return false;
    }
    file_path = playlists_[pidx].file_paths[saved_play_index_];
    

    // 优先按字节偏移恢复
    if (saved_file_offset_ > 0) {
        ESP_LOGI(TAG, "Resuming '%s' at offset %llu", file_path.c_str(), (unsigned long long)saved_file_offset_);
        return PlayFromSD(file_path, /*song_name*/"", saved_file_offset_);
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
            ESP_LOGI(TAG, "Approximated offset %llu (file_size=%zu duration_ms=%d)", (unsigned long long)approx_offset, file_size, duration_ms);
            return PlayFromSD(file_path, /*song_name*/"", approx_offset);
        }
    }

    // 否则从头开始播放
    ESP_LOGI(TAG, "Resume fallback: start from beginning of %s", file_path.c_str());
    return PlayFromSD(file_path, /*song_name*/"", 0);
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