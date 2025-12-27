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


#include "esp32_music.h"

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
    auto music = board.GetMusic();
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
        [&board,music](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            music->ResumePlayback();
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the lamp (0-100).",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight,music](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                music->ResumePlayback();
                return true;
            });
    }
    
    if (music) {
        AddTool("music.play",
                "当用户想要播放某个指定音乐时调用,从SD卡播放指定的本地音乐文件,你需要读出来要播放的音乐，然后调用完之后根据当前工具返回值来调用下一个工具，出现actually.2就调用工具actually.2，出现actually.1就调用工具actually.1\n"
                "参数:\n"
                "  `songname`: 要播放的歌曲名称,非必须,默认为空字符串。\n"
                "  `singer`: 歌手名称，可选，默认为空字符串。\n"
                "  `mode`: 播放模式，可选：`顺序播放`、`随机播放` 、 `循环播放`\n"
                "   `GoOn`: 继续播放标志位，默认为空字符串。\n"
                "返回:\n"
                "  播放状态信息，播报要播放的内容，并指示调用下一个工具actually.2或者actually.1。",
                PropertyList({
                    Property("songname", kPropertyTypeString,""), // 歌曲名称（可选）
                    Property("singer", kPropertyTypeString,""), // 歌手名称（可选）
                    Property("mode", kPropertyTypeString,"顺序播放"),// 播放模式（可选）
                    Property("GoOn", kPropertyTypeString,"")// 
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->SetMusicOrStory_(MUSIC);
                    auto song_name = properties["songname"].value<std::string>();
                    auto singer = properties["singer"].value<std::string>();
                    auto mode = properties["mode"].value<std::string>();
                    auto goon = properties["GoOn"].value<std::string>();
                    // JSON 字符串转义（局部 lambda）
                    auto escape_json = [](const std::string &s) {
                        std::string out;
                        out.reserve(s.size());
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
                        return out;
                    };
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

                    if((song_name.empty() && singer.empty()) || !goon.empty()) {
                        music->SetMode(true);
                        if(music->is_paused())
                        {
                            music->ResumePlayback();
                            return true;
                        }
                        if (music->IfSavedMusicPosition()) {
                            ESP_LOGI(TAG, "Resuming saved playback position");
                            std::string ai_instr_json = std::string("{\"call_tool\":\"actually.2\"}");
                            now_playing = music->GetCurrentSongName();
                            music->EnableRecord(true); 
                            ESP_LOGI(TAG, "Resuming song: %s", now_playing.c_str());
                            // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                            std::string payload = "{\"success\": true, \"message\": \"（简短播报一下）将为你继续播放\", \"now_playing\": \"";
                            payload += escape_json(now_playing);
                            payload += "\", \"ai_instruction\": ";
                            payload += ai_instr_json;
                            payload += "}";
                            return payload;
                        } else {
                            return "{\"success\": false, \"message\": \"没有保存的播放记录\"}";
                        }
                    }
                    else if(!song_name.empty() && singer.empty())
                    {
                        music->SetMode(true);
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
                            std::string ai_instr_json = std::string("{\"call_tool\":\"actually.1\"}");
                                                        
                            music->EnableRecord(true); 
                            // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                            std::string payload = "{\"success\": true, \"message\": \"（简短播报一下）将为你播放\", \"now_playing\": \"";
                            payload += escape_json(now_playing);
                            payload += "\", \"ai_instruction\": ";
                            payload += ai_instr_json;
                            payload += "}";
                            return payload;
                        }  else {
                            return "{\"success\": false, \"message\": \"未找到匹配的歌曲\"}";
                        }
                    }
                    else if(!singer.empty() && song_name.empty())
                    {
                        music->SetMode(true);
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
                        music->EnableRecord(true); 
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

                        // 构造 ai_instruction（机器可解析的调用指令：call_tool/play_music）
                        std::string ai_instr_json = std::string("{\"call_tool\":\"actually.1\"}");

                        // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                        std::string payload = "{\"success\": true, \"message\": \"读出来：将为你播放";
                        payload += escape_json(now_playing);
                        payload += "\", \"ai_instruction\": ";
                        payload += ai_instr_json;
                        payload += "}";
                        return payload;
                    }
                    else
                    {
                        music->SetMode(true);
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
                            music->EnableRecord(true); 
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
                    
                    return "{\"success\": true, \"message\": \"本地音乐开始播放\"}";
                });

        AddTool("actually.2",
        "调用完 music.play后，如果函数返回内容里出现{\"call_tool\":\"actually.2\"}，请立即再调用 actually.2，参数留空。",
        PropertyList(),
        [music](const PropertyList& properties) -> ReturnValue {
            // auto &app = Application::GetInstance();

            if (!music->ResumeSavedPlayback()) {
                ESP_LOGI(TAG, "ResumeSavedPlayback failed or not possible — fallback to random play");

                // 在库中随机选歌并播放，直到找到可播放的为止（若库为空则失败）
                size_t out_count = 0;
                auto all_music = music->GetMusicLibrary(out_count);
                if (out_count == 0) {
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
                        std::string payload = "{\"success\": true, \"message\": \"读出来：将为你播放";
                        payload += filename;
                        payload += "}";
                        return payload;

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
            auto &app = Application::GetInstance();

            auto list = music->GetCurrentPlayList();
            music->PlayPlaylist(list);
            return "{\"success\": true, \"message\": \"开始播放音乐\"}";
        });

        AddTool("searchmusic",
        "用于查询本地是否存在音乐，当用户主动询问哪些歌曲或者某个作者有哪些歌曲的时候调用\n"
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
            ESP_LOGI(TAG, "Search music: singer='%s', songname='%s'", singer.c_str(), song_name.c_str());
            size_t out_count = 0;
            // size_t max_count = 5;
            auto all_music = music->GetMusicLibrary(out_count);        
            // max_count = min(max_count, out_count);            
            std::string result = "{\"success\": true, \"message\": \"我可以播放以下歌曲: \", \"songs\": [";
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
                // 构建返回的 JSON 字符串
                for (size_t i = 0; i < hits.size(); ++i) {
                    result += "{\"title\": \"" + hits[i].first + "\", \"artist\": \"" + hits[i].second + "\"}";
                    if (i != hits.size() - 1) result += ", ";
                }
                result += "],需要我播放哪一首吗?,歌手: \"" + hits[0].second + "\"}";
            }
            else if(singer.empty() && !song_name.empty())
            {
                bool found = false;
                ESP_LOGI(TAG, "Search song: %s", song_name.c_str());
                auto index = music->SearchMusicIndexFromlist(song_name);
                if(index >=0 ) {
                    const char* path = all_music[index].file_path;
                    auto meta = ParseSongMeta(path);
                        // 找到匹配的歌曲和歌手
                        result += "{\"title\": \"" + meta.title + "\", \"artist\": \"" + meta.artist + "\"}";
                        found = true;
                }
                if (!found) {
                    return "{\"success\": false, \"message\": \"未找到匹配的歌曲和歌手\"}";
                }
                result += "],需要我播放吗?\"}";
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
                    // 找到匹配的歌曲和歌手
                    result += "{\"title\": \"" + meta.title + "\", \"artist\": \"" + meta.artist + "\"}";
                    found = true;
                }
                if (!found) {
                    return "{\"success\": false, \"message\": \"未找到匹配的歌曲和歌手\"}";
                }
                result += "],需要我播放吗?\"}";
            }
            else
            {
                size_t max_pick = 5;
                size_t total = music->GetMusicCount();
                size_t pick = std::min(max_pick, total);
                
                if (pick > 0) {

                    for (size_t k = 0; k < pick; ++k) {
                        size_t idx = esp_random() % pick;
                        const char* path = all_music[idx].file_path;
                        if (!path) continue;
                        auto meta = ParseSongMeta(path);
                        if (k) result += ", ";
                        result += "{\"title\": \"" + meta.title + "\", \"artist\": \"" + meta.artist + "\"}";
                    }
                }

                result += "],需要我播放哪一首吗?\"}";
            }
            return result;
        }
        );
                    
        AddTool("next",
                "当用户说要播放下一首歌或者下一章节故事或者下一个故事的时候调用，你需要读出来要播放的内容，然后调用完之后根据返回值，返回actually.1或者actually.3来播放下一首歌或者下一章节故事或者下一个故事\n"
                "参数:\n"
                "`mode`: 故事切换模式，`下一章`、`下一个`\n"
                "返回:\n"
                "返回下一个要调用的工具和要播放歌曲。",
                PropertyList({
                    Property("mode", kPropertyTypeString,"下一章") // 故事切换模式，下一章、下一个
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto MusicOrStory_ = music->GetMusicOrStory_();
                                        // JSON 字符串转义（局部 lambda）
                    auto escape_json = [](const std::string &s) {
                        std::string out;
                        out.reserve(s.size());
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
                        return out;
                    };

                    auto playmode = music->GetPlaybackMode();
                    std::string now_playing; // 要返回给调用方的播放提示
                    if(MusicOrStory_ == MUSIC)
                    {
                        if(music->is_paused())
                        {
                            music->StopStreaming(); // 停止当前播放
                        }
                        if(music->IfNodeIsEnd())
                        {                        
                            auto list = music->GetCurrentPlayList();
                            if(playmode == PLAYBACK_MODE_ORDER)
                                music->NextPlayIndexOrder(list);
                            else if(playmode == PLAYBACK_MODE_RANDOM)
                                music->NextPlayIndexRandom(list);
                            else if(playmode == PLAYBACK_MODE_LOOP)
                            {
                                // 在循环模式下，保持当前索引不变即可
                            }
                            now_playing = music->SearchMusicFromlistByIndex(list);
                            music->EnableRecord(true);
                        }
                        else
                        {
                            auto list = music->GetDefaultList();
                            music->SetPlayIndex(list, music->NextNodeIndex());
                            music->EnableRecord(false);
                            now_playing = music->SearchMusicFromlistByIndex(list);
                        }

                        size_t pos = now_playing.find_last_of("/\\");
                        if (pos != std::string::npos) 
                            now_playing = now_playing.substr(pos + 1);    
                        pos = now_playing.find_last_of('.');
                        if (pos != std::string::npos)
                            now_playing = now_playing.substr(0, pos);


                        std::string ai_instr_json = std::string("{\"call_tool\":\"actually.1\"}"); 
    
                        // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                        std::string payload = "{\"success\": true, \"message\": \"(你需要读出来)将为你播放\", \"now_playing\": \"";
                        payload += escape_json(now_playing);
                        payload += "\", \"ai_instruction\": ";
                        payload += ai_instr_json;
                        payload += "}";
                        return payload;
                    }
                    else
                    {
                        auto mode = properties["mode"].value<std::string>();

                        auto category = music->GetCurrentCategoryName();
                        auto story_name = music->GetCurrentStoryName();

                        if(category.empty() || story_name.empty())
                        {
                            return "{\"success\": false, \"message\": \"当前没有播放故事\"}";
                        }
                        if(mode == "下一章" || mode == "")
                        {
                            ESP_LOGI(TAG,"下一章");
                            if(!music->NextChapterInStory(category, story_name))
                            {
                                return "{\"success\": false, \"message\": \"下一章播放失败\"}";
                            }
                        }
                        else if(mode == "下一个")
                        {
                            ESP_LOGI(TAG,"下一个");
                            if(!music->NextStoryInCategory(category))
                            {
                                return "{\"success\": false, \"message\": \"下一个故事播放失败\"}";
                            }
                        }
                        std::string ai_instr_json = std::string("{\"call_tool\":\"actually.3\"}"); 
                        now_playing = music->GetCurrentCategoryName()+"故事："+music->GetCurrentStoryName()+
                                      "章节："+std::to_string(music->GetCurrentChapterIndex()+1);
                        // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                        std::string payload = "{\"success\": true, \"message\": \"(你需要读出来)将为你播放\", \"now_playing\": \"";
                        payload += escape_json(now_playing);
                        payload += "\", \"ai_instruction\": ";
                        payload += ai_instr_json;
                        payload += "}";
                        return payload;
                    }
                    return "{\"success\": false, \"message\": \"下一首播放失败\"}";
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
                [music](const PropertyList& properties) -> ReturnValue {
                    auto MusicOrStory_ = music->GetMusicOrStory_();
                                        // JSON 字符串转义（局部 lambda）
                    auto escape_json = [](const std::string &s) {
                        std::string out;
                        out.reserve(s.size());
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
                        return out;
                    };

                    std::string now_playing; // 要返回给调用方的播放提示
                    if(MusicOrStory_ == MUSIC)
                    {
                        if(music->is_paused())
                        {
                            music->StopStreaming(); // 停止当前播放
                        }
                        auto list = music->GetDefaultList();
                        auto index = music->LastNodeIndex();
                        if(index == -1)
                        {
                            return "{\"success\": false, \"message\": \"还没有播放记录呢，请先播放音乐\"}";
                        }

                        music->SetPlayIndex(list, index);
                        music->EnableRecord(false);
                        now_playing = music->SearchMusicFromlistByIndex(list);
                        

                        ESP_LOGI(TAG, "Last playing song: %s", now_playing.c_str());
                        size_t pos = now_playing.find_last_of("/\\");
                        if (pos != std::string::npos) 
                            now_playing = now_playing.substr(pos + 1);    
                        pos = now_playing.find_last_of('.');
                        if (pos != std::string::npos)
                            now_playing = now_playing.substr(0, pos);


                        std::string ai_instr_json = std::string("{\"call_tool\":\"actually.1\"}"); 
                        
                        // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                        std::string payload = "{\"success\": true, \"message\": \"(你需要读出来)将为你播放\", \"now_playing\": \"";
                        payload += escape_json(now_playing);
                        payload += "\", \"ai_instruction\": ";
                        payload += ai_instr_json;
                        payload += "}";
                        return payload;
                    }
                    else
                    {

                        // std::string ai_instr_json = std::string("{\"call_tool\":\"actually.3\"}"); 
                        // now_playing = music->GetCurrentCategoryName()+"故事："+music->GetCurrentStoryName()+
                        //               "章节："+std::to_string(music->GetCurrentChapterIndex()+1);
                        // // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                        // std::string payload = "{\"success\": true, \"message\": \"(你需要读出来)将为你播放\", \"now_playing\": \"";
                        // payload += escape_json(now_playing);
                        // payload += "\", \"ai_instruction\": ";
                        // payload += ai_instr_json;
                        // payload += "}";
                        // return payload;
                    }
                    return "{\"success\": false, \"message\": \"上一首播放失败\"}";
            });

            AddTool("story.search",
                    "用于查询本地的故事，当用户主动询问有哪些故事或者某个类别下有哪些故事或者问某个故事的章节的时候调用\n"
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
                            std::string result = "{\"success\": true, \"message\": \"我从故事库里面随机找了以下故事: \", \"stories\": [";
                            for (size_t i = 0; i < stories.size(); ++i) {
                                result += "\"" + stories[i] + "\"";
                                if (i != stories.size() - 1) result += ", ";
                            }
                            result += "]}";
                            return result;
                        }
                        else if(!story.empty() && category.empty())
                        {
                            // 先找到该故事所属类别及章节
                            auto cats = music->GetStoryCategories();
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
                            std::string result = "{\"success\": true, \"message\": \"我可以播放故事：" + final_name + "的以下章节: \", \"category\": \"" + found_cat + "\", \"chapters\": [";
                            for (size_t i = 0; i < chapters.size(); ++i) {
                                std::string ch = chapters[i];
                                size_t p = ch.find_last_of("/\\");
                                std::string name = (p == std::string::npos) ? ch : ch.substr(p + 1);
                                result += "\"" + name + "\"";
                                if (i != chapters.size() - 1) result += ", ";
                            }
                            result += "]}";
                            return result;
                        }
                        else if(story.empty() && category.empty())
                        {
                            // 未指定类别和故事：列出所有类别
                            auto cats = music->GetStoryCategories();
                            std::string result = "{\"success\": true, \"message\": \"我可以播放以下类别的故事: \", \"categories\": [";
                            for (size_t i = 0; i < cats.size(); ++i) {
                                result += "\"" + cats[i] + "\"";
                                if (i != cats.size() - 1) result += ", ";
                            }
                            result += "]}";
                            return result;
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
                            std::string result = "{\"success\": true, \"message\": \"我可以播放这个类别" + std::string(final_category) + "的故事:" + std::string(final_name) + "的以下章节: \", \"chapters\": [";
                            for (size_t i = 0; i < chapters.size(); ++i) {
                                std::string ch = chapters[i];
                                size_t p = ch.find_last_of("/\\");
                                std::string name = (p == std::string::npos) ? ch : ch.substr(p + 1);
                                result += "\"" + name + "\"";
                                if (i != chapters.size() - 1) result += ", ";
                            }
                            result += "]}";
                            return result;
                        }
                        return "{\"success\": false, \"message\": \"查询失败\"}";
                    });
                
            // story: 播放指定类别/故事/章节
            AddTool("story.play",
                "当用户说继续播放故事的时候调用。调用完之后根据当前工具返回值来调用下一个工具，出现actually.3就调用工具actually.3，出现actually.4就调用工具actually.4\n"
                "参数: \n"
                "`Category`:故事的类别,包括`搞笑`，`惊悚`，`益智`，可选\n"
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
                    Property("mode", kPropertyTypeString,"顺序播放")
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->SetMusicOrStory_(STORY);
                    auto cat = properties["Category"].value<std::string>();
                    auto name = properties["Story"].value<std::string>();
                    int chapter_idx = properties["Chapter_Index"].value<int>();
                    auto goon = properties["GoOn"].value<bool>();
                    auto mode = properties["mode"].value<std::string>();
                    ESP_LOGI(TAG, "story.play called with Category: %s, Story: %s, Chapter_Index: %d, GoOn: %d, mode: %s",
                             cat.empty()?"":cat.c_str(), name.empty()?"":name.c_str(), chapter_idx, goon, mode.c_str());
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
                    // JSON 字符串转义（局部 lambda）
                    auto escape_json = [](const std::string &s) {
                        std::string out;
                        out.reserve(s.size());
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
                        return out;
                    };

                    std::string now_playing; // 要返回给调用方的播放提示
                    if((cat.empty() && name.empty() && chapter_idx==0) || goon==true) {
                        // 播放上次的故事
                        if (music->IfSavedStoryPosition()) {
                            std::string ai_instr_json = std::string("{\"call_tool\":\"actually.4\"}"); 
                            now_playing = music->GetCurrentCategoryName()+"故事:"+music->GetCurrentStoryName()+" 第"+std::to_string(music->GetCurrentChapterIndex()+1)+"章";
                            // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                            std::string payload = "{\"success\": true, \"message\": \"读出来：将为你播放";
                            payload += escape_json(now_playing);
                            payload += "\", \"ai_instruction\": ";
                            payload += ai_instr_json;
                            payload += "}";
                            return payload;
                        } else {
                            return "{\"success\": false, \"message\": \"没有保存的播放记录\"}";
                        }
                    }
                    else
                    {
                        music->SetCurrentCategoryName(cat);
                        if(name.empty())
                        {
                            // 选择该类别下的第一个故事
                            auto stories = music->GetStoriesInCategory(cat);
                            if(stories.empty())
                            {
                                return "{\"success\": false, \"message\": \"该类别下没有故事\"}";
                            }
                            auto count = stories.size();
                            size_t idx = esp_random() % count;
                            name = stories[idx];
                        }
                        auto index = music->FindStoryIndexInCategory(cat, name); // 预加载章节列表
                        if(index == -1)
                        {
                            return "{\"success\": false, \"message\": \"未找到该故事\"}";
                        }
                        size_t  count;
                        auto storys = music->GetStoryLibrary(count);
                        auto final_name = storys[index].story_name;
                        music->SetCurrentStoryName(final_name);
                        if(chapter_idx == 0)
                        {
                            chapter_idx = 1; // 默认从第一章开始播放
                        }
                        music->SetCurrentChapterIndex(chapter_idx-1);
                        now_playing = cat + " 故事：" + final_name + " 第" + std::to_string(chapter_idx) + "章";
                        std::string ai_instr_json = std::string("{\"call_tool\":\"actually.3\"}"); 
                        
                        // 返回包含摘要 + 指示 AI 调用的 payload（ai_instruction 为 JSON 对象）
                        std::string payload = "{\"success\": true, \"message\": \"读出来：将为你播放";
                        payload += escape_json(now_playing);
                        payload += "\", \"ai_instruction\": ";
                        payload += ai_instr_json;
                        payload += "}";
                        return payload;

                    }

                });
            

            AddTool("actually.4",
                "调用完 story.play后，如果函数返回内容里出现{\"call_tool\":\"actually.4\"}，请立即再调用 actually.4，参数留空。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    if (!music->ResumeSavedStoryPlayback()) {
                        ESP_LOGI(TAG, "ResumeSavedStoryPlayback failed or not possible");
                    }
                    return "{\"success\": true, \"message\": \"继续播放故事\"}";
                });
            
            AddTool("actually.3",
                "调用完 story.play后，如果函数返回内容里出现{\"call_tool\":\"actually.3\"}，请立即再调用 actually.3，参数留空。\n"
                "返回：立刻开始播放，无需播报状态",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {

                    if(music->SelectStoryAndPlay())
                    {
                        return "{\"success\": true, \"message\": \"开始播放故事\"}";
                    }
                    return "{\"success\": false, \"message\": \"播放故事失败\"}";
                });
                }
            AddTool("stop",
                "当用户说停止播放音乐或者停止播放故事的时候调用，你需要立刻停止播放音乐或者故事，然后返回播放停止的状态",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->SetMode(false);
                    music->StopStreaming(); // 停止当前播放
                    return "{\"success\": true, \"message\": \"已停止播放\"}";
                });
            AddTool("Notice",
                "无",
                PropertyList(),
                [](const PropertyList& properties) -> ReturnValue {
                    auto &app = Application::GetInstance();
                    if(app.device_Role == Player)
                    {
                        
                        return "{\"success\": true, \"message\": \"系统提示：你现在是音乐播放器模式,需要快速播放音乐然后快速进入待机状态，根据用户需求调用相应工具来快速播放音乐，不需要与用户进行互动\"}";
                    }
                    else if(app.device_Role == Role_Xiaozhi)
                    {
                        return "{\"success\": true, \"message\": \"系统提示：你现在是AI助理模式，你的角色叫做小智，可以与用户进行深入的对话和互动，现在主动向用户进行问候吧\"}";
                    }
                    else if(app.device_Role == Role_ESP)
                    {
                        return "{\"success\": true, \"message\": \"系统提示：你现在是AI助理模式，你的角色叫做ESP，可以与用户进行深入的对话和互动，现在主动向用户进行问候吧\"}";
                    }
                    return "{\"success\": true, \"message\": \"系统提示：未知的设备角色\"}";
                });

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
