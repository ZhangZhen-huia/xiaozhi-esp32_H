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
    
    // AddTool("SayHello",
    //         "向用户问好时调用这个工具，告诉用户你现在的名字或者模式",
    //         PropertyList(),
    //         [](const PropertyList& properties) -> ReturnValue {
    //             auto &app = Application::GetInstance();
    //             std::string msg;
    //             if(app.device_Role == Role_Xiaozhi)
    //             {
    //                 msg = "角色提示：你现在的名字叫做小智，然后向用户介绍自己，并询问有什么需要帮助的";
    //             }
    //             else if(app.device_Role == Role_XiaoMing)
    //             {
    //                 msg = "角色提示：你现在的名字叫做小明，然后向用户介绍自己，并询问有什么需要帮助的";
    //             }
    //             else if(app.device_Role == Player)
    //             {
    //                 msg = "角色提示：你现在的模式是播放小助手，然后向用户介绍自己，并询问有什么需要帮助的";
    //             }
    //             return msg;
    //         });

    if (music) {
            AddTool("set_duration",
            "设置一个全局倒计时（秒）。若设置为 0 则取消定时器，与当前播放状态无关。",
            PropertyList({
                Property("seconds", kPropertyTypeInteger, 0, 0, 86400)
            }),
            [app,music](const PropertyList& properties) -> ReturnValue {
                int seconds = properties["seconds"].value<int>();
                if (seconds < 0) {
                    return std::string("{\"success\": false, \"message\": \"参数 seconds 必须 >= 0\"}");
                }
                if (seconds == 0) {
                    app->StopPlayDurationTimer();
                    ESP_LOGI(TAG, "music.set_play_duration: cancelled global timer");
                    return std::string("{\"success\": true, \"message\": \"已取消全局计时\"}");
                }

                // 确保正在播放（或恢复播放）
                if (music && music->is_paused()) {
                    music->ResumePlayback();
                }

                uint64_t us = static_cast<uint64_t>(seconds) * 1000000ULL;
                bool ok = app->CreateAndStartPlayTimer(us);
                if (ok) {
                    ESP_LOGI(TAG, "music.set_play_duration: set global countdown to %d seconds", seconds);
                    return std::string("{\"success\": true, \"message\": \"已设置全局剩余时长 " + std::to_string(seconds) + " 秒\"}");
                }
                ESP_LOGW(TAG, "music.set_play_duration: failed to start global timer");
                return std::string("{\"success\": false, \"message\": \"设置全局计时失败\"}");
            });

        AddTool("extend_duation",
            "延长当前全局计时（秒）。若当前没有计时器则自动从现在开始计时。",
            PropertyList({
                Property("extra", kPropertyTypeInteger, 0, 0, 86400)
            }),
            [app](const PropertyList& properties) -> ReturnValue {
                int extra = properties["extra"].value<int>();
                if (extra <= 0) {
                    return std::string("{\"success\": false, \"message\": \"参数 extra 必须大于 0\"}");
                }
                bool ok = app->ExtendPlayDurationSeconds(extra);
                if (ok) {
                    ESP_LOGI(TAG, "music.extend_play: extended global countdown by %d seconds", extra);
                    return std::string("{\"success\": true, \"message\": \"已延长全局计时 " + std::to_string(extra) + " 秒\"}");
                }
                return std::string("{\"success\": false, \"message\": \"无法延长全局计时\"}");
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

        AddTool("general.play",
                "用于播放本地的音乐或故事。当无具体类型时，将通过智能推断并播放。返回后续需要调用的 actually 工具。\n"
                "参数:\n"
                "  `name`: 歌曲名或故事名（可选）。注意，M或者S开头的并不是名字，而是故事或者音乐的索引\n"
                "  `target`: 明确播放类型，`music` 或 `story`（可选，未知留空）。\n"
                "  `mode`: 播放模式，`顺序`、`随机`、`循环`（可选）。\n"
                "  `GoOn`: 继续播放标志，`true` 继续当前/上次播放（可选）。\n"
                "  `duration`: 全局定时倒计时（秒），0为无限制。\n"
                "  `style`: 故事或者的风格/类别,音乐的类别：Classic nursery rhymes、Kids Learning Songs、Soothing Light Music、Natural Sounds\n"
                "  `Chapter_Index`: 仅限故事：要播放的章节号（默认1）。\n"
                "  `index_id`: 歌曲或者故事的编号，如M001、M1、s1等（可选）。\n",
                PropertyList({
                    Property("name", kPropertyTypeString, ""),
                    Property("target", kPropertyTypeString, ""),
                    Property("mode", kPropertyTypeString, ""),
                    Property("GoOn", kPropertyTypeBoolean, false),
                    Property("duration", kPropertyTypeInteger, 0, 0, 86400),
                    Property("style", kPropertyTypeString, ""),
                    Property("Chapter_Index", kPropertyTypeInteger, 1, 1, 1000),
                    Property("index_id", kPropertyTypeString, "")
                }),
                [music, &board, app](const PropertyList& properties) -> ReturnValue {
                    #if !my
                    #if battery_check
                    if(board.GetBatteryLevel() <= 10) {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放，请为设备充电后重试。\"}";
                    }
                    #endif
                    #endif
                    if (!music) return "{\"success\": false, \"message\": \"设备未初始化音频模块\"}";

                    auto name = properties["name"].value<std::string>();
                    auto target_raw = properties["target"].value<std::string>();
                    auto mode_str = properties["mode"].value<std::string>();
                    auto style_str = properties["style"].value<std::string>();
                    if (mode_str.empty())
                    {
                            if(app->GetDeviceFunction() != Function_Light) {
                                mode_str = "顺序";
                            } else {
                                mode_str = "循环";
                            }
                    }
                    
                    auto goon = properties["GoOn"].value<bool>();
                    int duration = properties["duration"].value<int>();
                    auto style = properties["style"].value<std::string>();
                    auto index_id = properties["index_id"].value<std::string>();
                    auto music_index_id = index_id;
                    int chapter_idx = properties["Chapter_Index"].value<int>();

                    if (duration > 0) app->Set_PlayDuration(duration);

                    auto music_impl = static_cast<Esp32Music*>(music);
                    
                    std::string target = NormalizeForSearch(target_raw);
                    bool force_music = (target == "music" || target == "音乐");
                    bool force_story = (target == "story" || target == "故事");
                    ESP_LOGE(TAG, "general.play called with name='%s', Music_index = '%s', style='%s'", name.c_str(), music_index_id.c_str(), style_str.c_str());

                    // 如果未明确指定目标，根据特有参数或模糊搜索进行推断
                    if (!force_music && !force_story) {
                        if (chapter_idx > 1) {
                            force_story = true;
                        } else if (goon) {
                            auto mos = music->GetMusicOrStory_();
                            if (mos == STORY) force_story = true;
                            else force_music = true;
                        } else if (!index_id.empty()) {
                            if (index_id[0] == 's' || index_id[0] == 'S') force_story = true;
                            else force_music = true;
                        } else if (!style.empty() && name.empty() && index_id.empty()) {
                            // style 有可能是音乐也有可能是故事，我们这里做模糊判断
                            auto hits = music_impl->FuzzySearchMedia(style, 1);
                            if (!hits.empty() && hits.front()->type == PSMediaType::kStory) {
                                force_story = true;
                            } else {
                                force_music = true;
                            }
                        } else if (!name.empty() || !style.empty()) {
                            std::string query;
                            if (!style.empty() && !name.empty()) query = style + "-" + name;
                            else if (!name.empty()) query = name;
                            else query = style;

                            auto hits = music_impl->FuzzySearchMedia(query, 1);
                            if (!hits.empty()) {
                                if (hits.front()->type == PSMediaType::kMusic) force_music = true;
                                else force_story = true;
                            } else {
                                force_music = true; // 无法分辨时暂时回退到音乐统一报错
                            }
                        } else {
                            force_music = true;
                        }
                    }

                    // 设置播放模式
                    std::string m = mode_str;
                    if (m.find("随机") != std::string::npos || m == "random" || m == "shuffle") {
                        music->SetRandomMode(true);
                        ESP_LOGI(TAG, "Set Random Play Mode");
                    } else if (m.find("循环") != std::string::npos || m == "loop") {
                        music->SetLoopMode(true);
                        ESP_LOGI(TAG, "Set Loop Play Mode");
                    } else {
                        music->SetOrderMode(true);
                        ESP_LOGI(TAG, "Set Order Play Mode");
                    }

                    std::string now_playing;

                    if (force_story) {
                        if(app->GetDeviceFunction() == Function_Light) {
                            return "{\"success\": false, \"message\": \"一键助眠模式无法播放故事呢\"}";
                        }
                        ESP_LOGE(TAG,"===================故事=======================");
                        // ==================== STORY 分支 ====================
                        std::string cat = style;
                        std::string story_name = name;
                        
                        if (!index_id.empty()) {
                            // i. 播放故事索引 "s1"
                            std::string search_idx = index_id;
                            if (search_idx[0] == 's' || search_idx[0] == 'S') {
                                std::string num_part = search_idx.substr(1);
                                try {
                                    int num = std::stoi(num_part);
                                    search_idx = "s" + std::to_string(num);
                                } catch (...) {}
                            } else {
                                try {
                                    int num = std::stoi(search_idx);
                                    search_idx = "s" + std::to_string(num);
                                } catch (...) {}
                            }
                            
                            size_t count = 0;
                            auto story_lib = music->GetStoryLibrary(count);
                            int found_idx = -1;
                            for (size_t i = 0; i < count; ++i) {
                                if (story_lib[i].index_id && std::string(story_lib[i].index_id) == search_idx) {
                                    found_idx = i;
                                    break;
                                }
                            }
                            if (found_idx >= 0) {
                                music->SetCurrentStoryIndex(found_idx);
                                music->SetCurrentCategoryName(story_lib[found_idx].category);
                                music->SetCurrentStoryName(story_lib[found_idx].story_name);
                                music->SetCurrentChapterIndex(std::max(0, chapter_idx - 1));
                                now_playing = std::string(story_lib[found_idx].story_name) + " 第" + std::to_string(chapter_idx) + "章";
                                return BuildNowPlayingPayload("{\"call_tool\":\"actually.3\"}", "读出来：将为你播放", now_playing);
                            } else {
                                return "{\"success\": false, \"message\": \"未找到对应编号的故事\"}";
                            }
                        } else if (!cat.empty() && story_name.empty()) {
                            // ii. 用户说播放大类，循环顺序播放该目录下的故事
                            music->SetCurrentCategoryName(cat);
                            auto stories = music->GetStoriesInCategory(cat);
                            if (stories.empty()) return "{\"success\": false, \"message\": \"该类别下没有故事\"}";
                            // 默认播放该类别第一个，或者随机
                            story_name = stories[0]; 
                            auto index = music->FindStoryIndexInCategory(cat, story_name);
                            if (index == -1) return "{\"success\": false, \"message\": \"未找到该故事\"}";
                            size_t count = 0;
                            auto storys = music->GetStoryLibrary(count);
                            music->SetCurrentStoryName(storys[index].story_name);
                            music->SetCurrentStoryIndex(index);
                            music->SetCurrentChapterIndex(std::max(0, chapter_idx - 1));
                            // 这里可以保持模式为顺序或者循环，外部已经设置
                            now_playing = cat + " 故事：" + storys[index].story_name + " 第" + std::to_string(chapter_idx) + "章";
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.3\"}", "读出来：将为你连续播放", now_playing);
                        } else if ((cat.empty() && story_name.empty() && chapter_idx <= 1) || goon) {
                            ESP_LOGI(TAG, "Continuing last story playback");
                            if (music->is_paused()) {
                                if (music->GetMusicOrStory_() == STORY) { music->ResumePlayback(); return true; }
                                else if (music->GetMusicOrStory_() == MUSIC) music->StopStreaming();
                            }
                            if (music->IfSavedStoryPosition()) {
                                now_playing = music->GetCurrentCategoryName() + "故事:" + music->GetCurrentStoryName() + " 第" + std::to_string(music->GetCurrentChapterIndex() + 1) + "章";
                                return BuildNowPlayingPayload("{\"call_tool\":\"actually.4\"}", "读出来：将为你播放", now_playing);
                            } else {
                                return "{\"success\": false, \"message\": \"没有保存的故事播放记录\"}";
                            }
                        } else {
                            // iii. 用户说某一故事的名字，或者大类+故事名
                            int index = -1;
                            if (cat.empty()) {
                                index = music->FindStoryIndexFuzzy(story_name);
                            } else {
                                index = music->FindStoryIndexInCategory(cat, story_name);
                            }
                            if (index == -1) return "{\"success\": false, \"message\": \"未找到该故事\"}";
                            size_t count = 0;
                            auto storys = music->GetStoryLibrary(count);
                            music->SetCurrentStoryIndex(index);
                            music->SetCurrentStoryName(storys[index].story_name);
                            music->SetCurrentCategoryName(storys[index].category);
                            music->SetCurrentChapterIndex(std::max(0, chapter_idx - 1));
                            now_playing = std::string(storys[index].category) + " 故事：" + storys[index].story_name + " 第" + std::to_string(chapter_idx) + "章";
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.3\"}", "读出来：将为你播放", now_playing);
                        }
                    } else {
                        // ==================== MUSIC 分支 ====================
                        std::string song_name = name;
                        ESP_LOGE(TAG,"===================音乐=======================");

                        auto check_light_mode_allowed = [](const char* cat, const char* m_idx = nullptr) {
                            if (m_idx && (strcmp(m_idx, "M0") == 0 || strcmp(m_idx, "m0") == 0)) return true;
                            if (!cat) return false;
                            auto my_strcasestr = [](const char* h, const char* n) -> bool {
                                if (!n || !*n) return true;
                                for (; *h; ++h) {
                                    if (tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                                        const char* h2 = h;
                                        const char* n2 = n;
                                        while (*h2 && *n2 && tolower((unsigned char)*h2) == tolower((unsigned char)*n2)) { ++h2; ++n2; }
                                        if (!*n2) return true;
                                    }
                                }
                                return false;
                            };
                            return my_strcasestr(cat, "sooth") || my_strcasestr(cat, "natural") || 
                                   strstr(cat, "舒缓") != nullptr || strstr(cat, "自然") != nullptr;
                        };

                        if (app->GetDeviceFunction() == Function_Light) {
                            ESP_LOGE(TAG, "index_id='%s'",music_index_id.c_str());

                            if (!music_index_id.empty()) {
                                // 允许直接播放包含该索引的音乐
                            } else if (!style.empty()) {
                                if (!check_light_mode_allowed(style.c_str())) {
                                    return "{\"success\": false, \"message\": \"夜灯模式下，只能播放舒缓音乐(Soothing Light Music)或自然声音(Natural Sounds)\"}";
                                }
                            } else if (song_name.empty() && music_index_id.empty()) {
                                style = esp_random() % 2 == 0 ? "Soothing Light Music" : "Natural Sounds";
                            }
                        }

                        if (!music_index_id.empty()) {
                            // 统一处理 index_id 标准化: M001 -> M1 等
                            std::string search_idx = music_index_id;
                            std::string num_part;
                            if (search_idx[0] == 'M' || search_idx[0] == 'm') {
                                num_part = search_idx.substr(1);
                                try {
                                    int num = std::stoi(num_part);
                                    search_idx = "M" + std::to_string(num);
                                } catch (...) {
                                }
                            } else {
                                try {
                                    int num = std::stoi(search_idx);
                                    search_idx = "M" + std::to_string(num);
                                } catch (...) {
                                }
                            }
                            if (music->is_paused()) music->StopStreaming();

                            size_t found_idx = -1;
                            auto musicInfo = music->FindMusicByIndexId(search_idx,&found_idx);

                            if (musicInfo != nullptr) {
                                auto playlist_name = music->GetDefaultList();
                                music->SetPlayIndex(playlist_name, found_idx);
                                music->SetCurrentPlayList(playlist_name);
                                music->EnableRecord(true, MUSIC);
                                auto meta = ParseSongMeta(musicInfo->file_path ? musicInfo->file_path : "");
                                std::string title_disp = meta.title.empty() ? search_idx : meta.title;
                                return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "（简短播报）将为你播放", title_disp);
                            } else {
                                uint8_t num = std::stoi(num_part);
                                if(num>0 && num<41 && app->GetDeviceFunction() == Function_Light) {
                                    return "{\"success\": false, \"message\": \"夜灯模式下，只能播放舒缓音乐(Soothing Light Music)或自然声音(Natural Sounds)，请尝试播放M0或者指定类别的音乐\"}";
                                }
                                return "{\"success\": false, \"message\": \"未找到对应编号的歌曲\"}";
                            }

                        } else if (!style.empty()) {
                            if (music->is_paused()) music->StopStreaming();
                            size_t lib_count = 0;
                            auto all_music = music->GetMusicLibrary(lib_count);
                            std::vector<std::string> file_paths;
                            for (size_t i = 0; i < lib_count; ++i) {
                                if (all_music[i].category && std::string(all_music[i].category).find(style) != std::string::npos) {
                                    if (all_music[i].file_path) {
                                        file_paths.emplace_back(all_music[i].file_path);
                                    }
                                }
                            }
                            if (file_paths.empty()) {
                                return "{\"success\": false, \"message\": \"未找到该类别(\" + style + \")的歌曲\"}";
                            }
                            
                            // 随机选择一个起始位置，保持歌曲前后的相对顺序不变（循环移位）
                            if (!file_paths.empty()) {
                                size_t start_idx = esp_random() % file_paths.size();
                                std::rotate(file_paths.begin(), file_paths.begin() + start_idx, file_paths.end());
                            }

                            music->EnableRecord(true, MUSIC);
                            std::string temp_playlist_name = "StyleList_" + style;
                            music->CreatePlaylist(temp_playlist_name, file_paths);
                            music->SetCurrentPlayList(temp_playlist_name);

                            auto meta = ParseSongMeta(file_paths.front());
                            now_playing = style + " 类别的歌曲，现在为你播放: " + (meta.title.empty() ? "第一首歌曲" : meta.title);
                            return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "读出来：将为你连续播放", now_playing);

                        } 
                        else if (song_name.empty()) {
                            if (music->is_paused()) {
                                if (music->GetMusicOrStory_() == MUSIC) { music->ResumePlayback(); return true; }
                                else if (music->GetMusicOrStory_() == STORY) music->StopStreaming();
                            }
                            if (music->GetPlaybackMode() == PLAYBACK_MODE_RANDOM && !goon) {
                                size_t lib_count = 0;
                                auto all_music = music->GetMusicLibrary(lib_count);
                                if (lib_count == 0) return "{\"success\": false, \"message\": \"音乐库为空，无法随机播放\"}";
                                size_t pick = static_cast<size_t>(esp_random() % lib_count);
                                const char* path = all_music[pick].file_path;
                                if (!path || !path[0]) return "{\"success\": false, \"message\": \"随机选曲失败\"}";

                                auto list = music->GetDefaultList();
                                music->SetCurrentPlayList(list);
                                music->SetPlayIndex(list, static_cast<int>(pick));
                                music->EnableRecord(true, MUSIC);

                                auto meta = ParseSongMeta(path);
                                now_playing = meta.title.empty() ? path : meta.title;

                                return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "（简短播报）将为你播放", now_playing);
                            }
                            if (music->IfSavedMusicPosition()) {
                                ESP_LOGI(TAG, "Resuming saved playback position");
                                now_playing = music->GetCurrentSongName();
                                music->EnableRecord(true, MUSIC);
                                ESP_LOGI(TAG, "Resuming song: %s", now_playing.c_str());
                                return BuildNowPlayingPayload("{\"call_tool\":\"actually.2\"}", "（简短播报）将为你继续播放", now_playing);
                            } else {
                                return "{\"success\": false, \"message\": \"没有保存的音乐播放记录\"}";
                            }

                        } 
                        else 
                        {
                            if (music->is_paused()) music->StopStreaming();
                            auto index = music->SearchMusicIndexFromlist(song_name);
                            if (index >= 0) {
                                size_t lib_count = 0;
                                auto all_music = music->GetMusicLibrary(lib_count);
                                if (app->GetDeviceFunction() == Function_Light && index < lib_count) {
                                    if (!check_light_mode_allowed(all_music[index].category)) {
                                        return "{\"success\": false, \"message\": \"夜灯模式下，只能播放舒缓音乐(Soothing Light Music)或自然声音(Natural Sounds)\"}";
                                    }
                                }
                                auto playlist_name = music->GetDefaultList();
                                music->SetPlayIndex(playlist_name, index);
                                music->SetCurrentPlayList(playlist_name);
                                music->EnableRecord(true, MUSIC);
                                return BuildNowPlayingPayload("{\"call_tool\":\"actually.1\"}", "（简短播报）将为你播放", song_name);
                            } else {
                                if(app->GetDeviceFunction() == Function_Light) {
                                    return "{\"success\": false, \"message\": \"夜灯模式下，只能播放舒缓音乐(Soothing Light Music)或自然声音(Natural Sounds)，请尝试指定类别的音乐\"}";
                                }
                                return "{\"success\": false, \"message\": \"未找到匹配的歌曲: \" + song_name}";
                            }
                        }
                        
                        return "{\"success\": false, \"message\": \"播放失败，未知参数组合\"}";
                    }
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
                    auto meta = ParseSongMeta(filename);
                    if (!meta.title.empty()) filename = meta.title;

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
                "  `category`: 音乐的类别：Classic nursery rhymes、Kids Learning Songs、Soothing Light Music、Natural Sounds\n"
                "  `index_id`: 歌曲编号，如M001、M1等（非必需）。\n"
                "返回:\n"
                "  返回可以播放的歌曲。",
                PropertyList({
                    Property("category", kPropertyTypeString,""), // 歌曲分类名称（可选）
                    Property("index_id", kPropertyTypeString,"") // 歌曲序号（可选）
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto category = properties["category"].value<std::string>();
                    auto index_id = properties["index_id"].value<std::string>();
                    size_t out_count = 0;
                    auto all_music = music->GetMusicLibrary(out_count);        
                    // 使用复用缓冲构建返回 JSON，避免多次小分配
                    g_mcp_scratch.clear();

                    if (!index_id.empty()) {
                        if (index_id[0] == 's') {
                            return "{\"success\": false, \"message\": \"编号以 S 开头的通常是故事，请尝试使用 searchstory 工具来搜索故事\"}";
                        } else {
                            auto info = music->FindMusicByIndexId(index_id);
                            if (info && info->song_name && info->song_name[0] != '\0') {
                                // 找到音乐，返回歌曲名称并询问是否播放
                                std::string response = "{\"success\": true, \"message\": \"找到了" + std::string(info->category) + "的音乐《" +
                                                        std::string(info->song_name) + 
                                                        "》，需要我帮你播放吗？\"}";
                                return response;
                            } else {
                                return "{\"success\": false, \"message\": \"未找到编号为 " + index_id + " 的音乐，请检查编号是否正确\"}";
                            }
                        }
                    }
                    else if(!category.empty())
                    {
                        auto music_list = music->SearchMusicByCategory(category);
                        size_t total = music_list.size();
                        size_t count = std::min<size_t>(total, 5);  // 最多返回5首
                        g_mcp_scratch = R"({"success": true, "message": "找到以下音乐", "songs": [)";
                        if (count > 0) {
                                // 随机采样索引
                                std::vector<size_t> indices(total);
                                std::iota(indices.begin(), indices.end(), 0);
                                for (size_t i = 0; i < count; ++i) {
                                    size_t j = i + (esp_random() % (total - i));
                                    std::swap(indices[i], indices[j]);
                                }
                                // 构建 JSON 数组
                                for (size_t k = 0; k < count; ++k) {
                                    if (k > 0) g_mcp_scratch += ',';
                                    const auto* info = music_list[indices[k]];
                                    g_mcp_scratch += "{\"name\":\"";
                                    g_mcp_scratch += (info->song_name ? info->song_name : "未知歌曲");
                                    g_mcp_scratch += "\", \"id\":\"";
                                    g_mcp_scratch += (info->index_id ? info->index_id : "");
                                    g_mcp_scratch += "\"}";
                                }
                            }

                            g_mcp_scratch += R"(], "ask": "需要我帮你播放哪一首？"})";
                            return g_mcp_scratch;
                    }                    
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
                            g_mcp_scratch += "\"}";
                        }
                    }

                    g_mcp_scratch += "],需要我播放哪一首吗?\"}";
                    return g_mcp_scratch;
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
                    #if battery_check
                    if(board.GetBatteryLevel() <= 10)
                    {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放，请为设备充电后重试。\"}";
                    }
                    #endif
                    #endif
                    auto MusicOrStory_ = music->GetMusicOrStory_();
                    auto playmode = music->GetPlaybackMode();
                    std::string now_playing; // 要返回给调用方的播放提示
                    
                    if (music->is_paused()) {
                        music->StopStreaming(); // 停止当前播放
                    }
                    if (MusicOrStory_ == MUSIC) {
                        music->SetManualNextPlay(true);
                        if (music->IfNodeIsEnd(MUSIC)) {
                            auto list = music->GetCurrentPlayList();
                            if (playmode == PLAYBACK_MODE_ORDER || playmode == PLAYBACK_MODE_LOOP)
                                music->NextPlayIndexOrder(list);
                            else if (playmode == PLAYBACK_MODE_RANDOM)
                                music->NextPlayIndexRandom(list);
                            now_playing = music->SearchMusicFromlistByIndex(list);
                            music->EnableRecord(true, MUSIC);
                        } else {
                            auto list = music->GetDefaultList();
                            music->SetPlayIndex(list, music->NextNodeIndex(MUSIC));
                            music->EnableRecord(false, MUSIC);
                            now_playing = music->SearchMusicFromlistByIndex(list);
                        }

                        auto meta = ParseSongMeta(now_playing);
                        std::string short_name = meta.title.empty() ? now_playing : meta.title;

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
                    #if battery_check
                    if(board.GetBatteryLevel() <= 10)
                    {
                        return "{\"success\": false, \"message\": \"当前电量过低，无法播放，请为设备充电后重试。\"}";
                    }
                    #endif
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
                        auto meta = ParseSongMeta(now_playing);
                        std::string short_name = meta.title.empty() ? now_playing : meta.title;

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
                    [this, music](const PropertyList& properties) -> ReturnValue {
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
        AddTool("actually.4",
                "调用完 story.play后，如果函数返回内容里出现{\"call_tool\":\"actually.4\"}，请立即再调用 actually.4，参数留空。",
                PropertyList(),
                [this, music, app](const PropertyList& properties) -> ReturnValue {
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
