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
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "lcd_display.h"
#define TAG "MCP"

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    //工具的名字，描述，属性表，回调函数
    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
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
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

    AddTool("self.led.turn_on",
            "Turn on the LED.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "Turn on the LED");
                return true;
            });
            
    AddTool("self.led.turn_off",
            "Turn off the LED.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "Turn off the LED");
                return true;
            });
            auto display = board.GetDisplay();
    // AddTool("self.music.open",
    //         "打开音乐播放器,当用户想要播放音乐或者打开音乐播放器的时候调用此工具",
    //         PropertyList(),
    //         [display](const PropertyList& properties) -> ReturnValue {
    //             if(display->music_screen_ == nullptr) {
    //                 display->MusicUI();
    //             }
                
    //             lv_obj_add_flag(display->main_screen_, LV_OBJ_FLAG_HIDDEN);
    //             lv_obj_clear_flag(display->music_screen_, LV_OBJ_FLAG_HIDDEN);
    //             return true;
    //         });
#ifdef HAVE_LVGL
    // auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif
    auto music = board.GetMusic();
    if (music) {
        // AddTool("self.music.play_song",
        //         "播放指定的歌曲。当用户要求播放音乐时使用此工具，会自动获取歌曲详情并开始流式播放。\n"
        //         "参数:\n"
        //         "  `song_name`: 要播放的歌曲名称（必需）。\n"
        //         "  `artist_name`: 要播放的歌曲艺术家名称（可选，默认为空字符串）。\n"
        //         "返回:\n"
        //         "  播放状态信息，不需确认，立刻播放歌曲。",
        //         PropertyList({
        //             Property("song_name", kPropertyTypeString),//歌曲名称（必需）
        //             Property("artist_name", kPropertyTypeString, "")//艺术家名称（可选，默认为空字符串）
        //         }),
        //         [music,display](const PropertyList& properties) -> ReturnValue {
        //         auto song_name = properties["song_name"].value<std::string>();
        //         auto artist_name = properties["artist_name"].value<std::string>();
        //          if (!music->Download(song_name, artist_name)) {
        //             return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
        //         }
        //         auto download_result = music->GetDownloadResult();
        //         ESP_LOGI(TAG, "Music details result: %s", download_result.c_str());
        //         if(music->WaitForMusicLoaded())
        //         {
        //             // 切换到在线音乐界面
        //             if(display->onlinemusic_screen_ == nullptr) {
        //                 display->OnlineMusicUI();
        //                 lv_obj_clear_flag(display->onlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
                        
        //             }
        //             if(display->current_screen_ != display->onlinemusic_screen_) {
        //                 lv_obj_add_flag(display->current_screen_, LV_OBJ_FLAG_HIDDEN);
        //                 lv_obj_clear_flag(display->onlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
        //             }
        //         } else {
        //             return "{\"success\": false, \"message\": \"音乐加载超时\"}";
        //         }
        //         return "{\"success\": true, \"message\": \"音乐开始播放\"}";
        //     });
        // AddTool("self.musicSDCard.play_song",
        //         "当用户想要播放某个指定音乐时调用,从SD卡播放指定的本地音乐文件。\n"
        //         "参数:\n"
        //         "  `songname`: 要播放的本地音乐文件名（必需）。\n"
        //         "  `mode`: 播放模式，可选：、`循环播放`、`播放一次`(默认)。\n"
        //         "返回:\n"
        //         "  播放状态信息，立刻开始播放。",
        //         PropertyList({
        //             Property("songname", kPropertyTypeString), // 本地音乐文件名（必需）
        //             Property("mode", kPropertyTypeString, "播放一次") // 播放模式（可选）
        //         }),
        //         [music,display](const PropertyList& properties) -> ReturnValue {
        //             auto name = properties["songname"].value<std::string>();
        //             std::string filepath = "/sdcard/音乐/" + name + ".mp3";
        //             ESP_LOGI(TAG, "Play local music file: %s", filepath.c_str());

        //             // 解析播放模式（支持中文与常见英文）
        //             auto mode_str = properties["mode"].value<std::string>();
        //             auto normalize = [](std::string s) {
        //                 // 简单去除首尾空白
        //                 while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
        //                 while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        //                 // 转小写（对英文有效，对中文无影响）
        //                 std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
        //                 return s;
        //             };
        //             std::string m = normalize(mode_str);
        //             if (m == "循环播放" || m == "循环" || m == "loop") {
        //                 music->SetLoopMode(true);
        //                 if (!music->PlayFromSD(filepath, name)) {
        //                     return "{\"success\": false, \"message\": \"播放本地音乐失败\"}";
        //                 }
        //             }
        //             else if(m=="播放一次" || m=="一次" || m=="once" || m=="single") 
        //             {
        //                 if (!music->PlayFromSD(filepath, name)) {
        //                     return "{\"success\": false, \"message\": \"播放本地音乐失败\"}";
        //                 }
        //             }
        //             return "{\"success\": true, \"message\": \"本地音乐开始播放\"}";
        //         });
        AddTool("playlist",
                "当用户没有指定播放歌曲时或者想要播放某个音乐歌单或者从哪个歌曲开始播放时调用此工具\n"
                "参数:\n"
                "  `playlist_name`: 要播放的播放列表名称,非必须,默认为默认歌单。\n"
                "  `start_song`: 从播放列表中的哪首歌开始播放,非必须,默认为空字符串，从第一首开始播放。\n"
                "  `mode`: 播放模式，可选：`顺序播放`、`随机播放` `单曲播放` `循环播放`\n"
                "返回:\n"
                "  播放状态信息，立刻开始播放。",
                PropertyList({
                    Property("playlist_name", kPropertyTypeString,""), // 播放列表名称（可选）
                    Property("start_song", kPropertyTypeString,""), // 从播放列表中的哪首歌开始播放（可选）
                    Property("mode", kPropertyTypeString,"顺序播放") // 播放模式（可选）
                }),
                [music,display](const PropertyList& properties) -> ReturnValue {
                    static int first_call = 1;
                    if(first_call && (properties["playlist_name"].value<std::string>().empty() &&
                       properties["start_song"].value<std::string>().empty())) {

                        ESP_LOGI(TAG, "Play Last Playlist and Index");
                        first_call = 0;
                        Settings settings("music", true); // 可写命名空间 "music"
                        auto last_playlist = settings.GetString("last_playlist", "DefaultMusicList");
                        music->SetPlayIndex(last_playlist, settings.GetInt("last_play_index", 0));
                        if(!music->PlayPlaylist(last_playlist))
                            return "{\"success\": false, \"message\": \"播放失败\"}";
                        music->SetCurrentPlayList(last_playlist);
                        return "{\"success\": true, \"message\": \"播放成功\"}";
                    }
                    else {
                        auto playlist_name = properties["playlist_name"].value<std::string>();
                        auto start_song = properties["start_song"].value<std::string>();
                        first_call = 0;
                        if(playlist_name.empty())
                            {
                                playlist_name = music->GetCurrentPlayList();
                                if(playlist_name.empty())
                                    playlist_name = "DefaultMusicList";
                            }
                        ESP_LOGI(TAG, "Play playlist: %s", playlist_name.c_str());
                        music->SetCurrentPlayList(playlist_name);
                        // 解析播放模式（支持中文与常见英文）
                        auto mode_str = properties["mode"].value<std::string>();
                        auto normalize = [](std::string s) {
                            // 简单去除首尾空白
                            while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
                            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
                            // 转小写（对英文有效，对中文无影响）
                            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
                            return s;
                        };
                        int index = 0;
                        std::string m = normalize(mode_str);
                        if(m == "随机播放" || m == "随机" || m == "shuffle" || m == "random") {
                            music->SetRandomMode(true);
                            ESP_LOGI(TAG, "Set Random Play Mode");
                        }
                        else if (m == "循环播放" || m== "循环" || m == "loop") {
                            music->SetLoopMode(true);
                            ESP_LOGI(TAG, "Set Loop Play Mode");
                        }
                        else if( m=="播放一次" || m=="一次" || m=="once" || m=="single" || m== "单曲播放") 
                        {
                            music->SetOnceMode(true);
                            ESP_LOGI(TAG, "Set Once Play Mode");
                        }
                        else {//默认顺序播放
                            music->SetOrderMode(true);
                            ESP_LOGI(TAG, "Set Order Play Mode");
                        }
                        //从指定音乐开始播放
                        if(!start_song.empty()) {
                                ESP_LOGI(TAG, "Start from song: %s", start_song.c_str());
                                auto index = music->SearchMusicIndexFromlist(start_song, playlist_name);
                                if(index >=0 ) {
                                    music->SetPlayIndex(playlist_name, index);
                                    if(!music->PlayPlaylist(playlist_name) )
                                        return "{\"success\": false, \"message\": \"播放失败\"}";
                                }
                        }
                        //这里就是从上次播放的音乐开始播放
                        else {
                                if(!music->PlayPlaylist(playlist_name) )
                                        return "{\"success\": false, \"message\": \"播放失败\"}";
                            }
                        return "{\"success\": true, \"message\": \"播放成功\"}";

                    }
                });
        AddTool("nextmusic",
                "下一首音乐播放工具\n"
                "返回:\n"
                "直接播放，不需要确认。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto list = music->GetCurrentPlayList();
                    auto mode = music->GetPlaybackMode();
                    if(mode == PLAYBACK_MODE_ORDER)
                        music->NextPlayIndexOrder(list);
                    else if(mode == PLAYBACK_MODE_RANDOM)
                        music->NextPlayIndexRandom(list);
                    else if(mode == PLAYBACK_MODE_LOOP)
                    {
                        // 在循环模式下，保持当前索引不变即可
                    }
                    if(music->PlayPlaylist(list))
                    {
                        return "{\"success\": true, \"message\": \"下一首播放成功\"}";
                    }
                    return "{\"success\": false, \"message\": \"下一首播放失败\"}";
            });
        AddTool("previousmusic",
                "上一首音乐播放工具\n"
                "返回:\n"
                "直接播放，不需要确认。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto list = music->GetCurrentPlayList();
                    auto mode = music->GetPlaybackMode();
                    int index = music->GetLastPlayIndex(list);
                    if(index >0)
                        music->SetPlayIndex(list, index);
                    else
                        music->SetPlayIndex(list, 0);

                    if(music->PlayPlaylist(list))
                    {
                        return "{\"success\": true, \"message\": \"上一首播放成功\"}";
                    }
                    return "{\"success\": false, \"message\": \"上一首播放失败\"}";
            });
        AddTool("addmusictolist",
                "把某个音乐加入某个歌单时调用\n"
                "参数:\n"
                "  `playlist_name`: 要添加到的播放列表名称,默认为默认歌单。(非必须)\n"
                "  `songname`: 要添加的本地音乐文件名，支持多个，用逗号分隔（非必需）。\n"
                "返回:\n"
                "  添加状态信息，立刻添加。",
                PropertyList({
                    Property("playlist_name", kPropertyTypeString,"DefaultMusicList"), // 播放列表名称（可选）
                    Property("songname", kPropertyTypeString,"") // 要添加的本地音乐文件名（必需）
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto playlist_name = properties["playlist_name"].value<std::string>();
                    
                    auto song_name = music->ExtractSongNameFromFileName(properties["songname"].value<std::string>());                    
                    if(!song_name.empty()) {
                        std::vector<std::string> filepaths;
                        std::stringstream ss(song_name);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            if(music->FindPlaylistIndex(playlist_name) == -1) {
                                // 歌单不存在，创建歌单
                                if(!music->CreatePlaylist(playlist_name)) {
                                    return "{\"success\": false, \"message\": \"创建歌单失败\"}";
                                }
                            }
                            auto name = music->SearchMusicPathFromlist(item, "DefaultMusicList");
                            //确保歌曲存在于音乐库中
                            if(name!= "")
                            {
                                filepaths.push_back(name);
                            }
                            else {
                                ESP_LOGW(TAG, "Music file %s not found in music library", item.c_str());
                                return "{\"success\": false, \"message\": \"音乐文件 " + item + " 不存在于音乐库中\"}";
                            }
                        }
                        music->AddMusicToPlaylist(playlist_name, filepaths);
                        return "{\"success\": true, \"message\": \"添加音乐到歌单成功\"}";
                    }
                    else {
                        // 仅创建歌单
                        if(music->CreatePlaylist(playlist_name)) {
                            return "{\"success\": true, \"message\": \"创建歌单成功\"}";
                        } else {
                            return "{\"success\": false, \"message\": \"创建歌单失败\"}";
                        }
                    }

                });

                // AddTool("SetPlayMode",
                //         "当需要更改或设置播放模式时调用此工具\n"
                //         "参数:\n"
                //         "  `mode`: 播放模式，可选：`顺序播放`、`随机播放`、`循环播放`、`播放一次`\n"
                //         "返回:\n"
                //         "  设置状态信息，立刻生效。",
                //         PropertyList({
                //             Property("mode", kPropertyTypeString,"顺序播放") // 播放模式（可选）
                //         }),
                //         [music](const PropertyList& properties) -> ReturnValue {
                //             // 解析播放模式（支持中文与常见英文）
                //             auto mode_str = properties["mode"].value<std::string>();
                //             auto normalize = [](std::string s) {
                //                 // 简单去除首尾空白
                //                 while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
                //                 while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
                //                 // 转小写（对英文有效，对中文无影响）
                //                 std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
                //                 return s;
                //             };
                //             std::string m = normalize(mode_str);
                //             if(m == "顺序播放" || m == "顺序" || m == "order") {
                //                 music->SetOrderMode(true);
                //             }
                //             else if(m == "随机播放" || m == "随机" || m == "shuffle" || m == "random") {
                //                 music->SetRandomMode(true);
                //             }
                //             else if (m == "循环播放" || m == "循环" || m == "loop") {
                //                 music->SetLoopMode(true);
                //             }
                //             else if(m=="播放一次" || m=="一次" || m=="once" || m=="single") 
                //             {
                //                 music->SetOnceMode(true);
                //             }
                //             return "{\"success\": true, \"message\": \"设置成功\"}";
                //     });
        // AddTool("self.music.completed",
        //         "在线音乐播放完成。",
        //         PropertyList(),
        //         [display](const PropertyList& properties) -> ReturnValue {
        //             if(display->current_screen_ == display->onlinemusic_screen_) {
        //                 lv_obj_add_flag(display->onlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
        //                 lv_obj_clear_flag(display->main_screen_, LV_OBJ_FLAG_HIDDEN);
        //             }
        //             return true;
        //     });
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

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

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
