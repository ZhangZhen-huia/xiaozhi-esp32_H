/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"

#include "board.h"
#include "settings.h"
#include <random>

#include "esp32_music.h"

#define TAG "MCP"
// 复用的线程本地缓冲与追加型转义，避免返回临时 string 导致频繁分配
static thread_local std::string g_mcp_scratch;
McpServer::McpServer() {
    g_mcp_scratch.reserve(4096);
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}


static void EscapeJsonAppend(const std::string &s, std::string &out) {
    out.reserve(out.size() + s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

// 复用缓冲构造标准化的 now_playing payload，ai_instr_json 可为 nullptr 表示不附加 ai_instruction 字段
static std::string BuildNowPlayingPayload(const char* ai_instr_json, const char* message_prefix, const std::string &now_playing) {
    g_mcp_scratch.clear();
    g_mcp_scratch.reserve(256 + now_playing.size());
    g_mcp_scratch += "{\"success\": true, \"message\": \"";
    g_mcp_scratch += message_prefix;
    g_mcp_scratch += "\", \"now_playing\": \"";
    EscapeJsonAppend(now_playing, g_mcp_scratch);
    g_mcp_scratch += "\"";
    if (ai_instr_json) {
        g_mcp_scratch += ", \"ai_instruction\": ";
        g_mcp_scratch += ai_instr_json;
    }
    g_mcp_scratch += "}";
    return g_mcp_scratch;
}

static std::string BuildNowPlayingResult(const char* call_tool, const std::string& now_playing, const std::string& speak_text) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "now_playing", now_playing.c_str());
    // 顶层供直接播放/展示的文本
    cJSON_AddStringToObject(root, "speak", speak_text.c_str());

    // ai_instruction 对象，供模型解析后调用工具并朗读
    cJSON *ai = cJSON_CreateObject();
    cJSON_AddStringToObject(ai, "call_tool", call_tool);        // 后续要调用的工具
    cJSON_AddStringToObject(ai, "speak", speak_text.c_str());   // 要朗读的文本
    cJSON_AddStringToObject(ai, "speak_type", "tts");          // 指示为 TTS 朗读
    cJSON_AddBoolToObject(ai, "should_speak", cJSON_True);     // 建议/强制模型先朗读
    cJSON_AddItemToObject(root, "ai_instruction", ai);

    char *out = cJSON_PrintUnformatted(root);
    std::string ret = out ? out : std::string("{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ret;
}
// // 全局保存最近一次请求的播放时长（秒），由 music.play 设置，由 actually.* 在开始播放时读取并启动定时器
// static std::atomic<int> g_requested_play_duration_sec{0};
// static esp_timer_handle_t* g_play_timer_handle = nullptr;
// static std::mutex g_play_timer_mutex;
// static std::atomic<int64_t> g_play_timer_expire_us{0};
// 启动一次性定时器（若之前存在则先取消）。在定时器回调中通过 Application::Schedule 在主线程调用停止播放。
// static void StartPlayDurationTimerIfRequested() {
//     int dur = g_requested_play_duration_sec.exchange(0);
//     if (dur <= 0) return;
//     ESP_LOGW(TAG, "Starting play duration timer for %d seconds", dur);
//     std::lock_guard<std::mutex> lock(g_play_timer_mutex);
//     if (g_play_timer_handle) {
//         esp_timer_stop(*g_play_timer_handle);
//         esp_timer_delete(*g_play_timer_handle);
//         delete g_play_timer_handle;
//         g_play_timer_handle = nullptr;
//     }

//     esp_timer_handle_t* th = new esp_timer_handle_t;
//     esp_timer_create_args_t args;
//     memset(&args, 0, sizeof(args));
//     args.callback = [](void* arg) {
//         // 在主线程停止播放，保证线程安全
//         Application::GetInstance().Schedule([=]() {
//             auto &board = Board::GetInstance();
//             auto m = board.GetMusic();
//             if (m) {
//                 ESP_LOGW(TAG, "Play duration timer expired, stopping playback");
//                 m->SetStopSignal(true);
//                 m->StopStreaming();
//                 m->SetMode(false);
//             }
//         });

//         // 清理定时器对象
//         esp_timer_handle_t* h = static_cast<esp_timer_handle_t*>(arg);
//         if (h && *h) {
//             esp_timer_stop(*h);
//             esp_timer_delete(*h);
//         }
//         delete h;

//         // 清全局指针与过期时间、请求时长
//         {
//             std::lock_guard<std::mutex> lk(g_play_timer_mutex);
//             g_play_timer_handle = nullptr;
//             g_play_timer_expire_us.store(0);
//             g_requested_play_duration_sec.store(0);
//         }
//     };
//     args.arg = th;
//     args.name = "play_duration_timer";

//     if (esp_timer_create(&args, th) != ESP_OK) {
//         delete th;
//         ESP_LOGW(TAG, "Failed to create play duration timer");
//         return;
//     }
//     g_play_timer_handle = th;
//     uint64_t us = static_cast<uint64_t>(dur) * 1000000ULL;
//     // 记录到期时间，供 ExtendPlayDurationSeconds 读取剩余时间
//     uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
//     g_play_timer_expire_us.store((int64_t)(now_us + us));
//     esp_timer_start_once(*th, us);
//     ESP_LOGI(TAG, "Started play duration timer: %d seconds (expire at %llu us)", dur, (unsigned long long)(now_us + us));
// }



// // 创建并启动一次性播放定时器（内部使用，调用时已持有互斥/线程安全）
// static bool CreateAndStartPlayTimer(uint64_t us) {
//     std::lock_guard<std::mutex> lock(g_play_timer_mutex);
//     // 清理旧定时器
//     if (g_play_timer_handle) {
//         esp_timer_stop(*g_play_timer_handle);
//         esp_timer_delete(*g_play_timer_handle);
//         delete g_play_timer_handle;
//         g_play_timer_handle = nullptr;
//     }

//     esp_timer_handle_t* th = new esp_timer_handle_t;
//     esp_timer_create_args_t args;
//     memset(&args, 0, sizeof(args));
//     args.callback = [](void* arg) {
//         // 在主线程停止播放，保证线程安全
//         Application::GetInstance().Schedule([=]() {
//             auto &board = Board::GetInstance();
//             auto m = board.GetMusic();
//             if (m) {
//                 ESP_LOGW(TAG, "Play duration timer expired, stopping playback");
//                 m->SetStopSignal(true);
//                 m->StopStreaming();
//                 m->SetMode(false);
//             }
//         });

//         // 清理定时器对象
//         esp_timer_handle_t* h = static_cast<esp_timer_handle_t*>(arg);
//         if (h && *h) {
//             esp_timer_stop(*h);
//             esp_timer_delete(*h);
//         }
//         delete h;

//         // 清全局指针与过期时间
//         std::lock_guard<std::mutex> lk(g_play_timer_mutex);
//         g_play_timer_handle = nullptr;
//         g_play_timer_expire_us.store(0);
//     };
//     args.arg = th;
//     args.name = "play_duration_timer";

//     if (esp_timer_create(&args, th) != ESP_OK) {
//         delete th;
//         ESP_LOGW(TAG, "Failed to create play duration timer");
//         return false;
//     }
//     g_play_timer_handle = th;
//     uint64_t now_us = (uint64_t)esp_timer_get_time();
//     g_play_timer_expire_us.store((int64_t)(now_us + us));
//     esp_timer_start_once(*th, us);
//     ESP_LOGI(TAG, "Started/Restarted play duration timer: %llu us", (unsigned long long)us);
//     return true;
// }

// // 在已有请求机制外，提供按秒延长播放时长的 API（线程安全）
// static bool ExtendPlayDurationSeconds(int extra_seconds) {
//     if (extra_seconds <= 0) return false;
//     uint64_t extra_us = static_cast<uint64_t>(extra_seconds) * 1000000ULL;

//     // 在持锁下采样当前定时器过期时间与是否存在定时器，计算剩余时间
//     uint64_t base_remaining_us = 0;
//     {
//         std::lock_guard<std::mutex> lock(g_play_timer_mutex);
//         uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
//         int64_t expire_us = g_play_timer_expire_us.load();
//         if (g_play_timer_handle && expire_us > static_cast<int64_t>(now_us)) {
//             base_remaining_us = static_cast<uint64_t>(expire_us - static_cast<int64_t>(now_us));
//             ESP_LOGI(TAG, "Extending existing play timer: +%d s, remaining %llu us",
//                      extra_seconds, (unsigned long long)base_remaining_us);
//         } else {
//             base_remaining_us = 0;
//             ESP_LOGI(TAG, "No existing play timer, creating new one for %d s", extra_seconds);
//         }
//     } // 释放锁 — 现在安全调用会再次锁的函数

//     uint64_t new_total_us = base_remaining_us + extra_us;
//     return CreateAndStartPlayTimer(new_total_us);
// }
bool NotResumePlayback = 0;

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    auto mcp = GetInstance();
    auto app = &Application::GetInstance();
    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    //工具的名字，描述，属性表，回调函数
    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, lamp, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),//没有参数，该工具不需要任何参数
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)//名字，类型，最小值0，最大值100
        }), 
        [&board,music](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            // 从 Application 获取唤醒到现在的已用时间（ms），并清除计时
            auto &app = Application::GetInstance();
            int64_t elapsed_ms = app.GetAndClearWakeElapsedMs();
            ESP_LOGI(TAG, "Elapsed ms since wake: %fs", elapsed_ms/1000.0);
            const int64_t kThresholdMs = 20000; // 小于该阈值视为短时间 -> 恢复播放
            if (elapsed_ms < kThresholdMs) {
                ESP_LOGI(TAG, "Resuming playback after volume adjustment");
                // 用户很快进行了亮度调整，恢复播放
                if(NotResumePlayback == 0)
                    music->ResumePlayback();
                
            } else {
                // 用户等待时间较长，不自动恢复；直接返回成功即可
                ESP_LOGI(TAG, "Brightness adjusted after %lld ms, not resuming playback", (long long)elapsed_ms);
                NotResumePlayback = 1;
            }

            return true;
        });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.lamp.set_brightness",
            "Set the brightness of the lamp (0-100).",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight,music](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);

                // 从 Application 获取唤醒到现在的已用时间（ms），并清除计时
                auto &app = Application::GetInstance();
                int64_t elapsed_ms = app.GetAndClearWakeElapsedMs();
                ESP_LOGI(TAG, "Elapsed ms since wake: %fs", elapsed_ms/1000.0);
                const int64_t kThresholdMs = 20000; // 小于该阈值视为短时间 -> 恢复播放
                if (elapsed_ms < kThresholdMs) {
                    // 用户很快进行了亮度调整，恢复播放
                    ESP_LOGI(TAG, "Resuming playback after brightness adjustment");
                    if(NotResumePlayback == 0)
                        music->ResumePlayback();
                        
                } else {
                    // 用户等待时间较长，不自动恢复；直接返回成功即可
                    ESP_LOGI(TAG, "Brightness adjusted after %lld ms, not resuming playback", (long long)elapsed_ms);
                    NotResumePlayback = 1;
                }

                return true;
            });
    }
    
    AddTool("SayHello",
            "向用户问好时调用这个工具，告诉用户你现在的名字或者模式",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto &app = Application::GetInstance();
                std::string msg;
                if(app.device_Role == Role_Xiaozhi)
                {
                    msg = "角色提示：你现在的名字叫做小智，然后向用户介绍自己，并询问有什么需要帮助的";
                }
                else if(app.device_Role == Role_XiaoMing)
                {
                    msg = "角色提示：你现在的名字叫做小明，然后向用户介绍自己，并询问有什么需要帮助的";
                }
                else if(app.device_Role == Player)
                {
                    msg = "角色提示：你现在的模式是播放小助手，然后向用户介绍自己，并询问有什么需要帮助的";
                }
                return msg;
            });


    if (music) {
        AddTool("music.set_play_duration",
                "设置当前播放的剩余时长（秒）。若设置为 0 则取消计时器。",
                PropertyList({
                    Property("seconds", kPropertyTypeInteger, 0, 0, 86400) // 0 表示取消计时器
                }),
                [music, app](const PropertyList& properties) -> ReturnValue {
                    int seconds = properties["seconds"].value<int>();
                    if (seconds < 0) {
                        return std::string("{\"success\": false, \"message\": \"参数 seconds 必须 >= 0\"}");
                    }

                    // 0 表示取消当前计时器
                    if (seconds == 0) {
                        app->StopPlayDurationTimer();
                        ESP_LOGI(TAG, "music.set_play_duration: cancelled play duration timer");
                        return std::string("{\"success\": true, \"message\": \"已取消播放计时\"}");
                    }

                    // 确保正在播放（或恢复播放）
                    if (music && music->is_paused()) {
                        music->ResumePlayback();
                    }

                    // 启动/重置定时器到指定剩余秒数
                    uint64_t us = static_cast<uint64_t>(seconds) * 1000000ULL;
                    bool ok = false;
                    // 优先使用 Application 对外的 API 创建定时器（CreateAndStartPlayTimer）
                    // 该函数在 Application 中实现为 CreateAndStartPlayTimer(uint64_t us)
                    // 若你的 Application API 名称不同，请改为相应的公开方法。
                    ok = app->CreateAndStartPlayTimer(us);

                    if (ok) {
                        ESP_LOGI(TAG, "music.set_play_duration: set remaining play time to %d seconds", seconds);
                        return std::string("{\"success\": true, \"message\": \"已设置播放剩余时长 " + std::to_string(seconds) + " 秒\"}");
                    } else {
                        ESP_LOGW(TAG, "music.set_play_duration: failed to set play timer");
                        return std::string("{\"success\": false, \"message\": \"设置播放时长失败\"}");
                    }
                });
        AddTool("music.extend_play",
                "延长当前播放的时长。参数: `extra`(秒)，表示在当前剩余时间基础上增加的秒数；若当前没有计时器则从现在开始计时。",
                PropertyList({
                    Property("extra", kPropertyTypeInteger, 0, 0, 86400) // 最小 1 秒，最大 86400 秒
                }),
                [music,app](const PropertyList& properties) -> ReturnValue {
                    int extra = properties["extra"].value<int>();
                    if (extra <= 0) {
                        return std::string("{\"success\": false, \"message\": \"参数 extra 必须大于 0\"}");
                    }
                    bool ok = app->ExtendPlayDurationSeconds(extra);
                    if (ok) {
                        music->ResumePlayback(); // 确保播放未暂停
                        return std::string("{\"success\": true, \"message\": \"已延长播放时长 " + std::to_string(extra) + " 秒\"}");
                    } else {
                        return std::string("{\"success\": false, \"message\": \"无法延长播放时长\"}");
                    }
                });
        AddTool("currentplay",
                "获取当前播放的音乐或者故事名字\n"
                "返回值：当前正在播放的内容",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {

                    if(music->ReturnMode())
                    {
                        if(music->GetMusicOrStory_() == MUSIC)
                        {
                            auto current_song = music->GetCurrentSongName();
                            return "{\"song\": \"" + current_song + "\"}";
                        }
                        else if(music->GetMusicOrStory_() == STORY)
                        {
                            auto current_story = music->GetCurrentStoryName();
                            auto current_chapter = music->GetCurrentChapterName();
                            return "{\"story\": \"" + current_story + "\", \"chapter\": \"" + current_chapter + "\"}";
                        }
                    }
                    return "当前没有在播放音乐或故事";
                });
        AddTool("stopplay",
                "当用户说停止播放的时候调用，你必须调用这个工具来停止当前的音乐播放。不能主观臆断当前状态",
                PropertyList(),
                [music,app](const PropertyList& properties) -> ReturnValue {
                    music->SetMode(false);
                    music->StopStreaming(); // 停止当前播放
                    // 取消播放计时器（如果存在）
                   app->StopPlayDurationTimer();
                    return true;
                });

    AddTool("music.create_style_playlist",
        "Create a temporary playlist from provided tracks and start continuous playback. "
        "参数: { \"tracks\": JSON 字符串数组 或 以逗号分隔的索引字符串 }. "
        "注意：本工具仅能在 music.play 工具返回并明确指示调用（通常为 music.play 返回的 ai_instruction 要求调用本工具）后由模型或客户端调用；"
        "若直接在未授权场景下调用，设备端可拒绝或忽略该调用。",
        PropertyList({
            Property("tracks", kPropertyTypeString, "") // JSON 数组字符串或 "1,2,3"
        }),
        [music](const PropertyList& properties) -> ReturnValue {
            std::string tracks_raw = properties["tracks"].value<std::string>();
            std::vector<std::string> tracks;
            // 解析 JSON 数组优先
            cJSON *arr = cJSON_Parse(tracks_raw.c_str());
            if (arr && cJSON_IsArray(arr)) {
                cJSON *it = nullptr;
                cJSON_ArrayForEach(it, arr) {
                    if (cJSON_IsString(it)) {
                        tracks.emplace_back(it->valuestring);
                    }
                }
                cJSON_Delete(arr);
            } else {
                // 尝试按逗号分割索引形式 "1,2,3"
                if (!tracks_raw.empty()) {
                    std::stringstream ss(tracks_raw);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        // token 可以是索引或路径；如果是纯数字我们可以从 music 库取 path
                        bool is_num = !token.empty() && std::all_of(token.begin(), token.end(), ::isdigit);
                        if (is_num) {
                            int idx = std::stoi(token);
                            size_t lib_count = 0;
                            auto all_music = music->GetMusicLibrary(lib_count);
                            if (idx >= 0 && (size_t)idx < lib_count) {
                                tracks.emplace_back(all_music[idx].file_path ? all_music[idx].file_path : std::string());
                            }
                        } else {
                            // 认为是路径或短名，直接加入
                            tracks.push_back(token);
                        }
                    }
                }
            }

            if (tracks.empty()) {
                return std::string("{\"success\":false,\"message\":\"no tracks provided\"}");
            }
            for(auto &track : tracks) {
                ESP_LOGI(TAG, "Selected track: %s", track.c_str());
            }
            // 在设备端创建播放列表并开始持续播放
            std::string tmp_name = "StylePlaylist_" + std::to_string(esp_random());
            music->CreatePlaylist(tmp_name, tracks);
            music->SetCurrentPlayList(tmp_name);
            music->EnableRecord(true, MUSIC);

            // 准备要读出来的第一首歌曲短名（去路径和扩展名）
            std::string now_playing;
            if (!tracks.empty()) {
                std::string first = tracks[0];
                size_t pos = first.find_last_of("/\\");
                if (pos != std::string::npos) first = first.substr(pos + 1);
                size_t dot = first.find_last_of('.');
                if (dot != std::string::npos) first = first.substr(0, dot);
                now_playing = first;
            } else {
                now_playing = tmp_name;
            }

            // 返回 payload，包含 ai_instruction 让模型调用 actually.1 来开始播放，并读出第一首
            return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "已创建并将为你播放：", now_playing);
        });

        AddTool("music.play",
                "用于播放某种风格的音乐,从SD卡播放指定的本地音乐文件,你需要读出来要播放的音乐，然后调用完之后根据当前工具返回值来调用下一个工具，出现actually.2就调用工具actually.2，出现actually.1就调用工具actually.1，仅仅用来播放音乐\n"
                "参数:\n"
                "  `songname`: 要播放的歌曲名称,非必须,默认为空字符串。\n"
                "  `singer`: 歌手名称，可选，默认为空字符串。\n"
                "  `mode`: 播放模式，可选：`顺序播放`、`随机播放` 、 `循环播放`\n"
                "   `GoOn`: 继续播放标志位，默认为空字符串。\n"
                "  `duration`: 播放时长（秒），可选，默认为 0，表示无限制。\n"
                "  `style`: 音乐风格，可选，默认为空字符串。\n"
                "返回:\n"
                "  播放状态信息，播报要播放的内容，并指示调用下一个工具actually.2或者actually.1。",
                PropertyList({
                    Property("songname", kPropertyTypeString,""), // 歌曲名称（可选）
                    Property("singer", kPropertyTypeString,""), // 歌手名称（可选）
                    Property("mode", kPropertyTypeString,"顺序播放"),// 播放模式（可选）
                    Property("GoOn", kPropertyTypeBoolean,false),// 
                    Property("duration", kPropertyTypeInteger, 0, 0, 86400), // 播放时长（秒），0 表示无限制
                    Property("style", kPropertyTypeString,"") //  音乐风格（可选）
                }),
                [music,&board,app](const PropertyList& properties) -> ReturnValue {
                    #if !my
                    if(board.GetBatteryLevel() <= 10)
                    {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放音乐，请为设备充电后重试。\"}";
                    }
                    #endif
                    auto song_name = properties["songname"].value<std::string>();
                    auto singer = properties["singer"].value<std::string>();
                    auto mode = properties["mode"].value<std::string>();
                    auto goon = properties["GoOn"].value<bool>();
                    auto style = properties["style"].value<std::string>();
                    int duration = properties["duration"].value<int>();
                    if (duration > 0) app->Set_PlayDuration(duration);
                    ESP_LOGW(TAG, "style: '%s'",style.c_str());
                    auto &app = Application::GetInstance();
                    // 解析播放模式（支持中文与常见英文）
                    auto mode_str = properties["mode"].value<std::string>();
                    std::string m = NormalizeForSearch(mode_str);
                    if(m == "随机播放" || m == "随机" || m == "shuffle" || m == "random") {
                        music->SetRandomMode(true);
                        ESP_LOGI(TAG, "Set Random Play Mode");
                    }
                    else if (m == "循环播放" || m== "循环" || m == "loop") {
                        music->SetLoopMode(true);
                        ESP_LOGI(TAG, "Set Loop Play Mode");
                    }
                    else {//默认顺序播放
                        music->SetOrderMode(true);
                        ESP_LOGI(TAG, "Set Order Play Mode");
                    }
                    std::string now_playing; // 要返回给调用方的播放提示
                    if (style != "") {
                        size_t lib_count = 0;
                        auto all_music = music->GetMusicLibrary(lib_count);
                        const size_t kMaxReturn = 50;

                        std::vector<size_t> indices;
                        indices.reserve(lib_count);
                        for (size_t i = 0; i < lib_count; ++i) indices.push_back(i);

                        if (lib_count > kMaxReturn) {
                            // 随机打乱并取前 kMaxReturn 个
                            std::default_random_engine rng((unsigned)esp_random());
                            std::shuffle(indices.begin(), indices.end(), rng);
                        }

                        size_t take = std::min(lib_count, kMaxReturn);
                        std::string lib_json = "[";
                        bool first = true;
                        for (size_t j = 0; j < take; ++j) {
                            size_t i = indices[j];
                            const char* path = all_music[i].file_path;
                            if (!path) continue;
                            auto meta = ParseSongMeta(path);
                            if (!first) lib_json += ",";
                            first = false;
                            lib_json += "{\"index\":";
                            lib_json += std::to_string(i);
                            lib_json += ",\"title\":\"";
                            EscapeJsonAppend(meta.title, lib_json);
                            lib_json += "\",\"artist\":\"";
                            EscapeJsonAppend(meta.artist, lib_json);
                            lib_json += "\",\"path\":\"";
                            EscapeJsonAppend(path, lib_json);
                            lib_json += "\"}";
                        }
                        lib_json += "]";

                        std::string ai_instr;
                        ai_instr.reserve(256 + lib_json.size() + style.size());
                        ai_instr += "{";
                        ai_instr += "\"call_tool\":\"music.create_style_playlist\",";
                        ai_instr += "\"style\":\"";
                        EscapeJsonAppend(style, ai_instr);
                        ai_instr += "\",";
                        ai_instr += "\"library\":";
                        ai_instr += lib_json;
                        ai_instr += ",";
                        ai_instr += "\"instruction\":\"请从 field 'library' 中选择多首最符合 style 的歌曲，返回并调用工具 music.create_style_playlist，工具参数为 { \\\"tracks\\\": <字符串数组或以逗号分隔的索引列表> }。至少返回 3 首歌曲用于连续播放。\"";
                        ai_instr += "}";

                        // 构造返回 JSON，包含 ai_instruction（供小智调用工具）
                        cJSON *root = cJSON_CreateObject();
                        cJSON_AddBoolToObject(root, "success", true);
                        cJSON_AddStringToObject(root, "now_playing", ("正在为你挑选 " + style + " 歌曲").c_str());
                        cJSON *ai = cJSON_CreateObject();
                        cJSON_AddStringToObject(ai, "call_tool", "music.create_style_playlist");
                        cJSON_AddStringToObject(ai, "instruction", ai_instr.c_str());
                        cJSON_AddStringToObject(ai, "speak", ("我会为你挑选并播放 " + style + " 风格的歌曲").c_str());
                        cJSON_AddItemToObject(root, "ai_instruction", ai);
                        char *out = cJSON_PrintUnformatted(root);
                        std::string ret = out ? out : std::string("{}");
                        if (out) cJSON_free(out);
                        cJSON_Delete(root);
                        return ret;
                    }
                    else if((song_name.empty() && singer.empty())) {
                        if(music->is_paused())
                        {
                            if(music->GetMusicOrStory_() == MUSIC)
                            {
                                music->ResumePlayback();
                                return true;
                            }
                            else if(music->GetMusicOrStory_() == STORY)
                            {
                                music->StopStreaming();
                            }
                        }
                        if (music->IfSavedMusicPosition()) {
                            ESP_LOGI(TAG, "Resuming saved playback position");
                            now_playing = music->GetCurrentSongName();
                            music->EnableRecord(true, MUSIC);
                            ESP_LOGI(TAG, "Resuming song: %s", now_playing.c_str());
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.2\"}", "（简短播报一下）将为你继续播放", now_playing);
                        } else {
                            return "{\"success\": false, \"message\": \"没有保存的播放记录\"}";
                        }
                    }
                    else if(!song_name.empty() && singer.empty())
                    {
                        if(music->is_paused())
                        {
                            music->StopStreaming(); // 停止当前播放
                        }
                        ESP_LOGI(TAG, "Playing song: %s", song_name.c_str());
                        auto index = music->SearchMusicIndexFromlist(song_name);
                        auto playlist_name = music->GetDefaultList();
                        if(index >=0 ) {
                            music->SetPlayIndex(playlist_name, index);
                            music->SetCurrentPlayList(playlist_name);
                            // 把找到的歌曲信息保存并返回，同时告诉 AI 调用另一个工具去播放
                            now_playing = song_name;
                            // 构造 ai_instruction（机器可解析的调用指令：call_tool/play_music）
                            music->EnableRecord(true, MUSIC);
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "（简短播报一下）将为你播放", now_playing);
                        }  else {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲\"}";
                        }
                    }
                    else if(!singer.empty() && song_name.empty())
                    {
                        if(music->is_paused())
                        {
                            music->StopStreaming(); // 停止当前播放
                        }
                        // 直接使用 PSRAM 中的音乐库，避免大拷贝
                        size_t out_count = 0;
                        auto all_music = (music)->GetMusicLibrary(out_count);
                        auto norm_singer = NormalizeForSearch(singer);
                        std::vector<std::string> file_paths;
                        for (size_t i = 0; i < out_count; ++i) {
                            const char* path = all_music[i].file_path;
                            if (!path) continue;
                            auto meta = ParseSongMeta(path);
                            if (meta.norm_artist.find(norm_singer) != std::string::npos) {
                                file_paths.emplace_back(path);
                            }
                        }
                        if (file_paths.empty()) {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲\"}";
                        }
                        music->EnableRecord(true, MUSIC);
                        // 创建临时播放列表
                        std::string temp_playlist_name = "SearchResults_" + singer;
                        music->CreatePlaylist(temp_playlist_name, file_paths);
                        music->SetCurrentPlayList(temp_playlist_name);

                        size_t pos = file_paths[0].find_last_of("/\\");
                        if (pos != std::string::npos) 
                            now_playing = file_paths[0].substr(pos + 1);

                        pos = now_playing.find_last_of('.');
                        if (pos != std::string::npos)
                            now_playing = now_playing.substr(0, pos);

                        ESP_LOGI(TAG, "Playing songs by singer: %s,->  %s", singer.c_str(),now_playing.c_str());


                        return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "读出来：将为你播放", now_playing);
                    }
                    else if(!singer.empty() && !song_name.empty())
                    {
                        music->SetMode(true);
                        music->SetMusicOrStory_(MUSIC);
                        if(music->is_paused())
                        {
                            music->StopStreaming(); // 停止当前播放
                        }
                        ESP_LOGI(TAG, "Playing song: %s by singer: %s", song_name.c_str(), singer.c_str());
                        // 直接在 PSRAM 中查找匹配项并播放
                        size_t out_count = 0;
                        auto all_music = static_cast<Esp32Music*>(music)->GetMusicLibrary(out_count);
                        bool found = false;
                        auto need_title = NormalizeForSearch(song_name);
                        auto need_artist = NormalizeForSearch(singer);
                        auto index = music->SearchMusicIndexFromlistByArtSong(need_title,need_artist);
                        const char* path = all_music[index].file_path;
                        auto meta = ParseSongMeta(path);
                        if (meta.norm_title == need_title && meta.norm_artist == need_artist) {
                            music->SetPlayIndex(music->GetDefaultList(), index);
                            music->EnableRecord(true, MUSIC);
                            // 找到匹配的歌曲和歌手
                            if (!music->PlayFromSD(path, song_name)) {
                                return "{\"success\": false, \"message\": \"播放失败\"}";
                            }
                            music->SetCurrentPlayList(music->GetDefaultList());
                            now_playing = singer + " - " + song_name;
                            found = true;
                        }
                        
                        if (!found) {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲和歌手\"}";
                        }

                    }
                    else if(goon)
                    {
                        if(music->is_paused())
                        {
                            music->ResumePlayback();
                            return true;
                        }
                        if (music->IfSavedMusicPosition()) {
                            ESP_LOGI(TAG, "Resuming saved playback position");
                            now_playing = music->GetCurrentSongName();
                            music->EnableRecord(true, MUSIC);
                            ESP_LOGI(TAG, "Resuming song: %s", now_playing.c_str());
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.2\"}", "（简短播报一下）将为你继续播放", now_playing);
                        } else {
                            return "{\"success\": false, \"message\": \"没有保存的播放记录\"}";
                        }
                    }
                    
                    return "{\"success\": true, \"message\": \"本地音乐开始播放\"}";
                });

        AddTool("actually.2",
        "调用完 music.play后，如果函数返回内容里出现{\"call_tool\":\"actually.2\"}，请立即再调用 actually.2，参数留空。",
        PropertyList(),
        [music,app](const PropertyList& properties) -> ReturnValue {
            // auto &app = Application::GetInstance();
            app->StartPlayDurationTimerIfRequested();
            music->SetMode(true);
            music->SetMusicOrStory_(MUSIC);
            if (!music->ResumeSavedPlayback()) {
                ESP_LOGI(TAG, "ResumeSavedPlayback failed or not possible — fallback to random play");
                // 在库中随机选歌并播放，直到找到可播放的为止（若库为空则失败）
                size_t out_count = 0;
                auto all_music = music->GetMusicLibrary(out_count);
                if (out_count == 0) {
                    music->SetMode(false);
                    ESP_LOGW(TAG, "actually.2 fallback: music library empty");
                    return "{\"success\": false, \"message\": \"无法继续播放音乐: 音乐库为空\"}";
                }

                // 随机循环直到找到可播放的条目（已知库非空，循环会终止）
                for (;;) {
                    size_t pick = (size_t)(esp_random() % out_count);
                    const char* path = all_music[pick].file_path;
                    if (!path) {
                        // 继续随机选择
                        continue;
                    }
                    // 尝试直接播放文件（PlayFromSD 应返回是否成功）
                    // 使用文件名作为展示名（不做复杂转义）
                    std::string filename = path;
                    size_t pos = filename.find_last_of("/\\");
                    if (pos != std::string::npos) filename = filename.substr(pos + 1);
                    size_t dot = filename.find_last_of('.');
                    if (dot != std::string::npos) filename = filename.substr(0, dot);

                    if (music->PlayFromSD(path, filename)) {
                        ESP_LOGI(TAG, "actually.2 fallback: playing random song: %s", filename.c_str());
                        return BuildNowPlayingPayload(nullptr, "读出来：将为你播放", filename);
                    } else {
                        ESP_LOGW(TAG, "actually.2 fallback: PlayFromSD failed for %s, retrying", path);
                        // 继续随机重试
                    }
                }
            }
            return "{\"success\": true, \"message\": \"继续播放音乐\"}";
        });

        AddTool("actually.1",
        "调用完 music.play或 next 后，如果函数返回内容里出现{\"call_tool\":\"actually.1\"}，请立即再调用 actually.1，参数留空。",
        PropertyList(),
        [music](const PropertyList& properties) -> ReturnValue {
            music->SetMusicOrStory_(MUSIC);
            auto &app = Application::GetInstance();
            auto list = music->GetCurrentPlayList();
            if(music->PlayPlaylist(list)) {
                music->SetMode(true);
                app.StartPlayDurationTimerIfRequested();
                return "{\"success\": true, \"message\": \"开始播放音乐\"}";
            }
            return "{\"success\": false, \"message\": \"播放音乐失败\"}";
        });

        AddTool("searchmusic",
                "用于查询本地是否存在音乐，当用户主动询问哪些歌曲或者某个作者有哪些歌曲或者问某个歌曲的时候调用，仅仅用来搜索音乐\n"
                "参数:\n"
                "  `singer`: 歌手名称（非必需）。\n"
                "   `songname`: 歌曲名称（非必需）。\n"
                "返回:\n"
                "  返回可以播放的歌曲。",
                PropertyList({
                    Property("singer", kPropertyTypeString,""), // 歌手名称（可选）
                    Property("songname", kPropertyTypeString,"") // 歌曲名称（可选）
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto singer = properties["singer"].value<std::string>();
                    auto song_name = properties["songname"].value<std::string>();
                    // ESP_LOGI(TAG, "Search music: singer='%s', songname='%s'", singer.c_str(), song_name.c_str());
                    size_t out_count = 0;
                    auto all_music = music->GetMusicLibrary(out_count);        
                    // 使用复用缓冲构建返回 JSON，避免多次小分配
                    g_mcp_scratch.clear();
                    if(!singer.empty() && song_name.empty()) {

                        ESP_LOGI(TAG, "Search songs by singer: %s", singer.c_str());
                        auto norm_singer = NormalizeForSearch(singer);
                        std::vector<std::pair<std::string,std::string>> hits; // (title, artist)
                        auto Searchresult = music->SearchMusicIndexBySingerRand5(norm_singer);
                        for(auto i : Searchresult)
                            {
                                const char* path = all_music[i].file_path;
                                if (!path) continue;
                                auto meta = ParseSongMeta(path);
                                if (meta.norm_artist.find(norm_singer) != std::string::npos) {
                                hits.emplace_back(meta.title, meta.artist);
                                ESP_LOGI(TAG, "Found song: %s by %s", meta.title.c_str(), meta.artist.c_str());
                                }
                            }

                        if (hits.empty()) {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲\"}";
                        }
                        // 构建返回的 JSON 字符串，使用 EscapeJsonAppend
                        g_mcp_scratch.reserve(512 + hits.size() * 64);
                        g_mcp_scratch += "{\"success\": true, \"message\": \"我可以播放以下歌曲: \", \"songs\": [";
                        for (size_t i = 0; i < hits.size(); ++i) {
                            if (i) g_mcp_scratch += ", ";
                            g_mcp_scratch += "{\"title\": \"";
                            EscapeJsonAppend(hits[i].first, g_mcp_scratch);
                            g_mcp_scratch += "\", \"artist\": \"";
                            EscapeJsonAppend(hits[i].second, g_mcp_scratch);
                            g_mcp_scratch += "\"}";
                        }
                        g_mcp_scratch += "],需要我播放哪一首吗?,歌手: \"";
                        EscapeJsonAppend(hits[0].second, g_mcp_scratch);
                        g_mcp_scratch += "\"}";
                        return g_mcp_scratch;
                    }
                    else if(singer.empty() && !song_name.empty())
                    {
                        bool found = false;
                        ESP_LOGI(TAG, "Search song: %s", song_name.c_str());
                        auto index = music->SearchMusicIndexFromlist(song_name);
                        if(index >=0 ) {
                            const char* path = all_music[index].file_path;
                            auto meta = ParseSongMeta(path);
                            // 找到匹配的歌曲和歌手，使用复用缓冲返回
                            g_mcp_scratch.reserve(256);
                            g_mcp_scratch = "{\"success\": true, \"message\": \"我可以播放以下歌曲: \", \"songs\": [";
                            g_mcp_scratch += "{\"title\": \"";
                            EscapeJsonAppend(meta.title, g_mcp_scratch);
                            g_mcp_scratch += "\", \"artist\": \"";
                            EscapeJsonAppend(meta.artist, g_mcp_scratch);
                            g_mcp_scratch += "\"}],需要我播放吗?\"}";
                            found = true;
                        }
                        if (!found) {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲和歌手\"}";
                        }
                        return g_mcp_scratch;
                    }
                    else if(!singer.empty() && !song_name.empty())
                    {
                        ESP_LOGI(TAG, "Search song: %s by singer: %s", song_name.c_str(), singer.c_str());
                        auto need_title = NormalizeForSearch(song_name);
                        auto need_artist = NormalizeForSearch(singer);
                        auto index = music->SearchMusicIndexFromlistByArtSong(need_title,need_artist);
                        bool found = false;
                        if(index >=0 )
                        {
                            const char* path = all_music[index].file_path;
                            auto meta = ParseSongMeta(path);
                            // 找到匹配的歌曲和歌手，使用复用缓冲返回
                            g_mcp_scratch.reserve(256);
                            g_mcp_scratch = "{\"success\": true, \"message\": \"我可以播放以下歌曲: \", \"songs\": [";
                            g_mcp_scratch += "{\"title\": \"";
                            EscapeJsonAppend(meta.title, g_mcp_scratch);
                            g_mcp_scratch += "\", \"artist\": \"";
                            EscapeJsonAppend(meta.artist, g_mcp_scratch);
                            g_mcp_scratch += "\"}],需要我播放吗?\"}";
                            found = true;
                        }
                        if (!found) {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲和歌手\"}";
                        }
                        return g_mcp_scratch;
                    }
                    else
                    {
                        size_t max_pick = 5;
                        size_t total = music->GetMusicCount();
                        size_t pick = std::min(max_pick, total);
                        
                        // 列出随机歌曲
                        g_mcp_scratch.reserve(512);
                        g_mcp_scratch = "{\"success\": true, \"message\": \"我可以播放以下歌曲: \", \"songs\": [";
                        if (pick > 0) {
                            for (size_t k = 0; k < pick; ++k) {
                                size_t idx = esp_random() % total;
                                const char* path = all_music[idx].file_path;
                                if (!path) continue;
                                auto meta = ParseSongMeta(path);
                                if (k) g_mcp_scratch += ", ";
                                g_mcp_scratch += "{\"title\": \"";
                                EscapeJsonAppend(meta.title, g_mcp_scratch);
                                g_mcp_scratch += "\", \"artist\": \"";
                                EscapeJsonAppend(meta.artist, g_mcp_scratch);
                                g_mcp_scratch += "\"}";
                            }
                        }

                        g_mcp_scratch += "],需要我播放哪一首吗?\"}";
                        return g_mcp_scratch;
                    }
                    return "{\"success\": false, \"message\": \"查询失败\"}";
                }
                );
                    
        AddTool("next",
                "当用户说要播放下一首歌或者下一章节故事或者下一个故事的时候调用，你需要读出来要播放的内容，然后调用完之后根据返回值，返回actually.1或者actually.3来播放下一首歌或者下一章节故事或者下一个故事\n"
                "参数:\n"
                "`mode`: 故事切换模式，`下一章`、`下一个`、`换一个`，你需要仔细判断用户说的什么要求，是下一章节还是下一个故事\n"
                "返回:\n"
                "返回下一个要调用的工具和要播放歌曲。",
                PropertyList({
                    Property("mode", kPropertyTypeString,"下一个") // 故事切换模式，下一章、下一个
                }),
                [music,&board](const PropertyList& properties) -> ReturnValue {
                    #if !my
                    if(board.GetBatteryLevel() <= 10)
                    {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放，请为设备充电后重试。\"}";
                    }
                    #endif

                    auto MusicOrStory_ = music->GetMusicOrStory_();
                    auto playmode = music->GetPlaybackMode();
                    std::string now_playing; // 要返回给调用方的播放提示

                    if (music->is_paused()) {
                        music->StopStreaming(); // 停止当前播放
                    }
                    if (MusicOrStory_ == MUSIC) {
                        if (music->IfNodeIsEnd(MUSIC)) {
                            auto list = music->GetCurrentPlayList();
                            if (playmode == PLAYBACK_MODE_ORDER)
                                music->NextPlayIndexOrder(list);
                            else if (playmode == PLAYBACK_MODE_RANDOM)
                                music->NextPlayIndexRandom(list);
                            else if (playmode == PLAYBACK_MODE_LOOP) {
                                // 循环模式：保持当前索引
                            }
                            now_playing = music->SearchMusicFromlistByIndex(list);
                            music->EnableRecord(true, MUSIC);
                        } else {
                            auto list = music->GetDefaultList();
                            music->SetPlayIndex(list, music->NextNodeIndex(MUSIC));
                            music->EnableRecord(false, MUSIC);
                            now_playing = music->SearchMusicFromlistByIndex(list);
                        }

                        size_t pos = now_playing.find_last_of("/\\");
                        std::string short_name = (pos != std::string::npos) ? now_playing.substr(pos + 1) : now_playing;
                        pos = short_name.find_last_of('.');
                        if (pos != std::string::npos) short_name = short_name.substr(0, pos);

                        std::string speak = "下一首歌是：" + short_name + "。";
                        return BuildNowPlayingResult("actually.1", short_name, speak);
                    } else {
                        auto mode = properties["mode"].value<std::string>();
                        auto category = music->GetCurrentCategoryName();
                        auto story_name = music->GetCurrentStoryName();

                        if (music->IfNodeIsEnd(STORY)) {
                            ESP_LOGW(TAG, "=============%s===============",mode.c_str());
                            if (category.empty() || story_name.empty()) {
                                return std::string("{\"success\": false, \"message\": \"当前没有播放故事\"}");
                            }
                            if (mode == "下一章" || mode == "下一集" || mode == "" || mode.find("个") == std::string::npos || mode.find("章") != std::string::npos || mode.find("集") != std::string::npos) {
                                if (!music->NextChapterInStory(category, story_name)) {
                                    return std::string("{\"success\": false, \"message\": \"下一章播放失败\"}");
                                }
                            } else if (mode == "下一个" || mode == "换一个" || mode.find("个") != std::string::npos) {
                                if (!music->NextStoryInCategory(category)) {
                                    return std::string("{\"success\": false, \"message\": \"下一个故事播放失败\"}");
                                }
                            }
                            music->EnableRecord(true, STORY);
                            now_playing = music->GetCurrentCategoryName() + "故事：" + music->GetCurrentStoryName() +
                                        "，章节:" + music->GetCurrentChapterName();
                            std::string speak = "接下来为你播放" + now_playing + "。";
                            return BuildNowPlayingResult("actually.3", now_playing, speak);
                        } else {
                            size_t count = 0;
                            auto ps_story_index_ = music->GetStoryLibrary(count);
                            music->EnableRecord(false, STORY);
                            size_t idx = music->NextNodeIndex(STORY);
                            music->SetCurrentStoryName(ps_story_index_[idx].story_name);
                            music->SetCurrentCategoryName(ps_story_index_[idx].category);
                            now_playing = music->GetCurrentCategoryName() + "故事：" + music->GetCurrentStoryName() +
                                        "，章节:" + music->GetCurrentChapterName();
                            std::string speak = "下一则故事是" + now_playing + "。";
                            return BuildNowPlayingResult("actually.3", now_playing, speak);
                        }
                    }

                    return std::string("{\"success\": false, \"message\": \"下一首播放失败\"}");
                });
        AddTool("last",
                "当用户说要播放上一首歌或者上一章节故事或者上一个故事的时候调用，你需要读出来要播放的内容，然后调用完之后根据返回值，返回actually.1或者actually.3来播放上一首歌或者上一章节故事或者上一个故事\n"
                "参数:\n"
                "`mode`: 故事切换模式，`上一章`、`上一个`\n"
                "返回:\n"
                "返回下一个要调用的工具和要播放歌曲。",
                PropertyList({
                    Property("mode", kPropertyTypeString,"上一章") // 故事切换模式，上一章、上一个
                }),
                [music,&board](const PropertyList& properties) -> ReturnValue {
                    #if !my
                    if(board.GetBatteryLevel() <= 10)
                    {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放，请为设备充电后重试。\"}";
                    }
                    #endif
                    auto MusicOrStory_ = music->GetMusicOrStory_();
                                        
                    std::string now_playing; // 要返回给调用方的播放提示
                    if (music->is_paused()) {
                        music->StopStreaming(); // 停止当前播放
                    }

                    if (MusicOrStory_ == MUSIC) {
                        auto list = music->GetDefaultList();
                        auto index = music->LastNodeIndex(MUSIC);
                        if (index == -1) {
                            return std::string("{\"success\": false, \"message\": \"还没有播放记录呢，请先播放音乐\"}");
                        }

                        music->SetPlayIndex(list, index);
                        music->EnableRecord(false, MUSIC);
                        now_playing = music->SearchMusicFromlistByIndex(list);

                        ESP_LOGI(TAG, "Last playing song: %s", now_playing.c_str());
                        size_t pos = now_playing.find_last_of("/\\");
                        std::string short_name = (pos != std::string::npos) ? now_playing.substr(pos + 1) : now_playing;
                        pos = short_name.find_last_of('.');
                        if (pos != std::string::npos) short_name = short_name.substr(0, pos);

                        std::string speak = "上一首歌是：" + short_name + "。";
                        return BuildNowPlayingResult("actually.1", short_name, speak);
                    } else {
                        music->EnableRecord(false, STORY);
                        auto index = music->LastNodeIndex(STORY);
                        if (index == -1) {
                            return std::string("{\"success\": false, \"message\": \"还没有播放记录呢，请先播放故事\"}");
                        }

                        size_t count = 0;
                        auto ps_story_index_ = music->GetStoryLibrary(count);
                        music->SetCurrentStoryName(ps_story_index_[index].story_name);
                        music->SetCurrentCategoryName(ps_story_index_[index].category);
                        auto chapters = music->GetCurrentChapterName();
                        size_t pos  = chapters.find_last_of("/\\");
                        if (pos != std::string::npos) {
                            chapters = chapters.substr(pos + 1);
                        }
                        pos = chapters.find_last_of('.');
                        if (pos != std::string::npos) {
                            chapters = chapters.substr(0, pos);
                        }
                        now_playing = music->GetCurrentCategoryName() + "故事：" + music->GetCurrentStoryName() +
                                        "章节：" + chapters;

                        std::string speak = "上一则为你播放的是" + now_playing + "。";
                        return BuildNowPlayingResult("actually.3", now_playing, speak);
                    }

                    return std::string("{\"success\": false, \"message\": \"上一首播放失败\"}");
                });

            AddTool("story.search",
                    "用于查询本地的故事，当用户主动询问有哪些故事或者某个类别下有哪些故事或者问某个故事的章节的时候调用,你需要先查询到完整的故事路径然后再播放\n"
                    "参数:\n"
                    "  `category`: 故事类别（可选）。\n"
                    "  `story`: 故事名称（可选）。\n"
                    "返回:\n"
                    "  返回可以播放的故事或者章节。",
                    PropertyList({
                        Property("category", kPropertyTypeString,""), // 故事类别（可选）
                        Property("story", kPropertyTypeString,"") // 故事名称（可选）
                    }),
                    [music](const PropertyList& properties) -> ReturnValue {
                        auto category = properties["category"].value<std::string>();
                        auto story = properties["story"].value<std::string>();
                        ESP_LOGI(TAG, "Search story: category='%s', story='%s'", category.c_str(), story.c_str());
                        if(!category.empty() && story.empty())
                        {
                            auto stories = music->GetStoriesInCategory(category);
                            if(stories.empty())
                            {
                                return "{\"success\": false, \"message\": \"该类别下没有故事\"}";
                            }
                            // 使用复用缓冲构建返回
                            g_mcp_scratch.clear();
                            g_mcp_scratch.reserve(256 + stories.size() * 32);
                            g_mcp_scratch += "{\"success\": true, \"message\": \"我从故事库里面随机找了以下故事: \", \"stories\": [";
                            for (size_t i = 0; i < stories.size(); ++i) {
                                if (i) g_mcp_scratch += ", ";
                                g_mcp_scratch += "\"";
                                EscapeJsonAppend(stories[i], g_mcp_scratch);
                                g_mcp_scratch += "\"";
                            }
                            g_mcp_scratch += "]}";
                            return g_mcp_scratch;
                        }
                        else if(!story.empty() && category.empty())
                        {
                            std::string found_cat;
                            auto index = music->FindStoryIndexFuzzy(story);  
                            
                            size_t count = 0;
                            auto storys = music->GetStoryLibrary(count);
                            std::string final_name;
                            if(index != -1)
                            {
                                found_cat = storys[index].category;
                                final_name = storys[index].story_name;
                            }
                            else
                            {
                                return "{\"success\": false, \"message\": \"未找到该故事\"}";
                            }
                            auto chapters = music->GetChaptersForStory(found_cat, final_name);
                            if (chapters.empty()) {
                                return "{\"success\": false, \"message\": \"未找到该故事或该故事没有章节\"}";
                            }
                            g_mcp_scratch.clear();
                            g_mcp_scratch.reserve(256 + chapters.size()*32);
                            g_mcp_scratch += "{\"success\": true, \"message\": \"我可以播放故事：";
                            EscapeJsonAppend(final_name, g_mcp_scratch);
                            g_mcp_scratch += "的以下章节: \", \"category\": \"";
                            EscapeJsonAppend(found_cat, g_mcp_scratch);
                            g_mcp_scratch += "\", \"chapters\": [";
                            for (size_t i = 0; i < chapters.size(); ++i) {
                                if (i) g_mcp_scratch += ", ";
                                std::string ch = chapters[i];
                                size_t p = ch.find_last_of("/\\");
                                std::string name = (p == std::string::npos) ? ch : ch.substr(p + 1);
                                // 去掉文件扩展名（例如 .mp3 .wav 等）
                                size_t dot = name.find_last_of('.');
                                if (dot != std::string::npos) name = name.substr(0, dot);
                                g_mcp_scratch += "\"";
                                EscapeJsonAppend(name, g_mcp_scratch);
                                g_mcp_scratch += "\"";
                            }
                            g_mcp_scratch += "]}";
                            return g_mcp_scratch;
                        }
                        else if(story.empty() && category.empty())
                        {
                            // 未指定类别和故事：列出所有类别
                            auto cats = music->GetStoryCategories();
                            g_mcp_scratch.clear();
                            g_mcp_scratch.reserve(256 + cats.size()*32);
                            g_mcp_scratch += "{\"success\": true, \"message\": \"我可以播放以下类别的故事: \", \"categories\": [";
                            for (size_t i = 0; i < cats.size(); ++i) {
                                if (i) g_mcp_scratch += ", ";
                                g_mcp_scratch += "\"";
                                EscapeJsonAppend(cats[i], g_mcp_scratch);
                                g_mcp_scratch += "\"";
                            }
                            g_mcp_scratch += "]}";
                            return g_mcp_scratch;
                        }
                        else if(!story.empty() && !category.empty())
                        {
                            auto index = music->FindStoryIndexInCategory(category, story);
                            if(index == -1)
                            {
                                return "{\"success\": false, \"message\": \"该类别下没有该故事\"}";
                            }
                            size_t count = 0;
                            auto storys = music->GetStoryLibrary(count);
                            auto final_name = storys[index].story_name;
                            auto final_category = storys[index].category;
                            auto chapters = music->GetChaptersForStory(final_category, final_name);
                            if(chapters.empty())
                            {
                                return "{\"success\": false, \"message\": \"该类别下没有该故事或该故事没有章节\"}";
                            }
                            g_mcp_scratch.clear();
                            g_mcp_scratch.reserve(256 + chapters.size()*32);
                            g_mcp_scratch += "{\"success\": true, \"message\": \"我可以播放这个类别";
                            EscapeJsonAppend(final_category, g_mcp_scratch);
                            g_mcp_scratch += "的故事:";
                            EscapeJsonAppend(final_name, g_mcp_scratch);
                            g_mcp_scratch += "的以下章节: \", \"chapters\": [";
                            for (size_t i = 0; i < chapters.size(); ++i) {
                                if (i) g_mcp_scratch += ", ";
                                std::string ch = chapters[i];
                                size_t p = ch.find_last_of("/\\");
                                std::string name = (p == std::string::npos) ? ch : ch.substr(p + 1);
                                // 去掉文件扩展名（例如 .mp3 .wav 等）
                                size_t dot = name.find_last_of('.');
                                if (dot != std::string::npos) name = name.substr(0, dot);
                                g_mcp_scratch += "\"";
                                EscapeJsonAppend(name, g_mcp_scratch);
                                g_mcp_scratch += "\"";
                            }
                            g_mcp_scratch += "]}";
                            return g_mcp_scratch;
                        }
                        return "{\"success\": false, \"message\": \"查询失败\"}";
                    });
                
            // story: 播放指定类别/故事/章节
            AddTool("story.play",
                "用于播放故事。先用story.search找到到完整的故事路径，调用完之后根据当前工具返回值来调用下一个工具，出现actually.3就调用工具actually.3，出现actually.4就调用工具actually.4。\n"
                "参数: \n"
                "`Category`:故事的类别,可选\n"
                " `Story`:故事名称,可选\n"
                " `Chapter_Index`:故事章节,(可选，默认0)\n"
                " `GoOn`: 继续播放上次的故事标志位，默认为`false`。\n"
                " `mode`: 播放模式，有`随机`、`循环`和`顺序`三种，默认为`顺序`。\n"
                "返回:\n"
                "  播放状态信息，播报要播放的内容，并指示调用下一个工具actually.3或者actually.4。",
                PropertyList({
                    Property("Category", kPropertyTypeString, ""),
                    Property("Story", kPropertyTypeString, ""),
                    Property("Chapter_Index", kPropertyTypeInteger, 0, 0, 1000),
                    Property("GoOn", kPropertyTypeBoolean,false),
                    Property("mode", kPropertyTypeString,"顺序播放"),
                    Property("duration", kPropertyTypeInteger, 0, 0, 86400), // 播放时长（秒），0 表示无限制
                }),
                [music,&board,app](const PropertyList& properties) -> ReturnValue {
                    #if !my
                    if(board.GetBatteryLevel() <= 10)
                    {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放，请为设备充电后重试。\"}";
                    }
                    #endif
                    auto cat = properties["Category"].value<std::string>();
                    auto name = properties["Story"].value<std::string>();
                    int chapter_idx = properties["Chapter_Index"].value<int>();
                    auto goon = properties["GoOn"].value<bool>();
                    auto mode = properties["mode"].value<std::string>();
                    int duration = properties["duration"].value<int>();
                    if (duration > 0) app->Set_PlayDuration(duration);
                    ESP_LOGI(TAG, "story.play called with Category: %s, Story: %s, Chapter_Index: %d, GoOn: %d, mode: %s, duration: %d",
                             cat.empty()?"":cat.c_str(), name.empty()?"":name.c_str(), chapter_idx, goon, mode.c_str(), duration);
                    // 解析播放模式（支持中文与常见英文）
                    mode = NormalizeForSearch(mode);
                    if(mode == "随机播放" || mode == "随机" || mode == "shuffle" || mode == "random") {
                        music->SetRandomMode(true);
                        ESP_LOGI(TAG, "Set Random Play Mode for Story");
                    }
                    else if (mode == "循环播放" || mode== "循环" || mode == "loop") {
                        music->SetLoopMode(true);
                        ESP_LOGI(TAG, "Set Loop Play Mode for Story");
                    }
                    else {//默认顺序播放
                        music->SetOrderMode(true);
                        ESP_LOGI(TAG, "Set Order Play Mode for Story");
                    }

                    std::string now_playing; // 要返回给调用方的播放提示
                    if((cat.empty() && name.empty() && chapter_idx==0)) {
                        ESP_LOGI(TAG, "Continuing last story playback");
                        if(music->is_paused())
                        {
                            if(music->GetMusicOrStory_() == STORY)
                            {
                                music->ResumePlayback();
                                return true;
                            }
                            else if(music->GetMusicOrStory_() == MUSIC)
                            {
                                music->StopStreaming(); // 停止当前播放
                            }

                        }
                        // 播放上次的故事
                        if (music->IfSavedStoryPosition()) {
                            now_playing = music->GetCurrentCategoryName() + "故事:" + music->GetCurrentStoryName() + " 第" + std::to_string(music->GetCurrentChapterIndex() + 1) + "章";
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.4\"}", "读出来：将为你播放", now_playing);
                        } else {
                            return "{\"success\": false, \"message\": \"没有保存的播放记录\"}";
                        }
                    }
                    else if(cat.empty() && !name.empty() )
                    {
                        std::string found_cat;
                        auto index = music->FindStoryIndexFuzzy(name);  
                        
                        size_t count = 0;
                        auto storys = music->GetStoryLibrary(count);
                        std::string final_name;
                        if(index != -1)
                        {
                            found_cat = storys[index].category;
                            final_name = storys[index].story_name;
                        }
                        else
                        {
                            return "{\"success\": false, \"message\": \"未找到该故事\"}";
                        }
                        music->SetCurrentStoryIndex(index);
                        music->SetCurrentCategoryName(found_cat);
                        music->SetCurrentStoryName(final_name);
                        if(chapter_idx == 0)
                        {
                            chapter_idx = 1; // 默认从第一章开始播放
                        }
                        music->SetCurrentChapterIndex(chapter_idx-1);
                        now_playing = cat + " 故事：" + final_name + " 第" + std::to_string(chapter_idx) + "章";
                        return BuildNowPlayingPayload("{\"call_tool\":\"actually.3\"}", "读出来：将为你播放", now_playing);
                    }
                    else if(!cat.empty() && name.empty())
                    {
                        //这样就是在这个类别里面循环播放
                        music->SetCurrentCategoryName(cat);
                        // 选择该类别下的第一个故事
                        auto stories = music->GetStoriesInCategory(cat);
                        if(stories.empty())
                        {
                            return "{\"success\": false, \"message\": \"该类别下没有故事\"}";
                        }
                        auto count = stories.size();
                        size_t idx = esp_random() % count;
                        name = stories[idx];

                        auto index = music->FindStoryIndexInCategory(cat, name); // 预加载章节列表
                        if(index == -1)
                        {
                            return "{\"success\": false, \"message\": \"未找到该故事\"}";
                        }
                        auto storys = music->GetStoryLibrary(count);
                        auto final_name = storys[index].story_name;
                        music->SetCurrentStoryName(final_name);
                        chapter_idx = 1; // 从第一章开始播放
                        music->SetCurrentStoryIndex(index);
                        music->SetCurrentChapterIndex(chapter_idx-1);
                        now_playing = cat + " 故事：" + final_name + " 第" + std::to_string(chapter_idx) + "章";
                        return BuildNowPlayingPayload("{\"call_tool\":\"actually.3\"}", "读出来：将为你播放", now_playing);
                    }
                    else if(!cat.empty() && !name.empty())
                    {
                       auto index = music->FindStoryIndexInCategory(cat, name); // 预加载章节列表
                        if(index == -1)
                        {
                            return "{\"success\": false, \"message\": \"未找到该故事\"}";
                        }
                        size_t count = 0;
                        auto storys = music->GetStoryLibrary(count);
                        auto final_name = storys[index].story_name;
                        music->SetCurrentStoryIndex(index);
                        music->SetCurrentStoryName(final_name);
                        music->SetCurrentCategoryName(cat);
                        if(chapter_idx <=0 )
                            chapter_idx = 1;

                        
                        auto chapters = music->GetCurrentChapterName();
                        size_t pos = chapters.find_last_of("/\\");
                        if(pos != std::string::npos) 
                            chapters = chapters.substr(pos + 1);
                        pos = chapters.find_last_of('.');
                        if(pos != std::string::npos)
                            chapters = chapters.substr(0, pos);

                        if(chapter_idx == 0)
                        {
                            chapter_idx = 1; // 默认从第一章开始播放
                        }

                        music->SetCurrentChapterIndex(chapter_idx-1);
                        now_playing = cat + " 故事：" + final_name + " 第" + std::to_string(chapter_idx) + "章" + chapters;
                        return BuildNowPlayingPayload("{\"call_tool\":\"actually.3\"}", "读出来：将为你播放", now_playing);

                    }
                    else if( goon == true)
                    {
                        ESP_LOGI(TAG, "Continuing last story playback");
                        if(music->is_paused())
                        {
                            music->ResumePlayback();
                            return true;
                        }
                        // 播放上次的故事
                        if (music->IfSavedStoryPosition()) {
                            now_playing = music->GetCurrentCategoryName() + "故事:" + music->GetCurrentStoryName() + " 第" + std::to_string(music->GetCurrentChapterIndex() + 1) + "章";
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.4\"}", "读出来：将为你播放", now_playing);
                        } else {
                            return "{\"success\": false, \"message\": \"没有保存的播放记录\"}";
                        }
                    }
                    return "{\"success\": false, \"message\": \"播放故事失败\"}";
                });
            
        AddTool("actually.4",
                "调用完 story.play后，如果函数返回内容里出现{\"call_tool\":\"actually.4\"}，请立即再调用 actually.4，参数留空。",
                PropertyList(),
                [music,app](const PropertyList& properties) -> ReturnValue {
                    ESP_LOGI(TAG, "actually.4 called to resume story playback");
                    music->SetMusicOrStory_(STORY);

                    // 先尝试断点恢复播放
                    if (music->ResumeSavedStoryPlayback()) {
                        app->StartPlayDurationTimerIfRequested();
                        music->SetMode(true);
                        return "{\"success\": true, \"message\": \"播放故事成功\"}";
                    }

                    // 断点恢复失败，回退到从库中随机选一个故事，从第一章开始播放
                    ESP_LOGI(TAG, "ResumeSavedStoryPlayback failed — fallback to random story");
                    size_t count = 0;
                    auto story_index = music->GetStoryLibrary(count);
                    if (count == 0) {
                        music->SetMode(false);
                        ESP_LOGW(TAG, "actually.4 fallback: story library empty");
                        return "{\"success\": false, \"message\": \"无法播放故事: 故事库为空\"}";
                    }

                    size_t pick = static_cast<size_t>(esp_random()) % count;
                    // 设置选中的故事并从第1章开始
                    music->SetCurrentStoryIndex(static_cast<int>(pick));
                    music->SetCurrentCategoryName(story_index[pick].category);
                    music->SetCurrentStoryName(story_index[pick].story_name);
                    music->SetCurrentChapterIndex(0);

                    // 尝试播放选中的故事
                    if (music->SelectStoryAndPlay()) {
                        app->StartPlayDurationTimerIfRequested();
                        music->SetMode(true);
                        std::string now_playing = music->GetCurrentCategoryName() + "故事:" + music->GetCurrentStoryName() + " 第" + std::to_string(music->GetCurrentChapterIndex() + 1) + "章";
                        return BuildNowPlayingPayload("{\"call_tool\":\"actually.4\"}", "读出来：将为你播放", now_playing);

                    } else {
                        music->SetMode(false);
                        return "{\"success\": false, \"message\": \"播放故事失败\"}";
                    }
                });
          
            AddTool("actually.3",
                "调用完 story.play后，如果函数返回内容里出现{\"call_tool\":\"actually.3\"}，请立即再调用 actually.3，参数留空。\n"
                "返回：立刻开始播放，无需播报状态",
                PropertyList(),
                [music,app](const PropertyList& properties) -> ReturnValue {
                    ESP_LOGI(TAG, "actually.3 called to start story playback");
                    music->SetMusicOrStory_(STORY);
                    if(music->SelectStoryAndPlay())
                    {
                        app->StartPlayDurationTimerIfRequested();
                        music->SetMode(true);
                        return "{\"success\": true, \"message\": \"开始播放故事\"}";
                    }
                    return "{\"success\": false, \"message\": \"播放故事失败\"}";
                });
                }


    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
            
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                auto ota = std::make_unique<Ota>();
                
                bool success = app.UpgradeFirmware(*ota, url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
