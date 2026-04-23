import re

with open('main/mcp_server.cc', 'r') as f:
    text = f.read()

# We want to replace the block starting from:
# force_music = 1;
# force_story = 0;
# ... all the way to:
#                     } else {
#                     {
#                         // ==================== MUSIC 分支 ====================

begin_str = """                    force_music = 1;
                    force_story = 0;"""

end_str = """                    // } else {
                    {
                        // ==================== MUSIC 分支 ===================="""

new_logic = """
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
                            return "{\\"success\\": false, \\"message\\": \\"当前模式无法播放故事\\"}";
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
                                return BuildNowPlayingPayload("{\\"call_tool\\":\\"actually.3\\"}", "读出来：将为你播放", now_playing);
                            } else {
                                return "{\\"success\\": false, \\"message\\": \\"未找到对应编号的故事\\"}";
                            }
                        } else if (!cat.empty() && story_name.empty()) {
                            // ii. 用户说播放大类，循环顺序播放该目录下的故事
                            music->SetCurrentCategoryName(cat);
                            auto stories = music->GetStoriesInCategory(cat);
                            if (stories.empty()) return "{\\"success\\": false, \\"message\\": \\"该类别下没有故事\\"}";
                            // 默认播放该类别第一个，或者随机
                            story_name = stories[0]; 
                            auto index = music->FindStoryIndexInCategory(cat, story_name);
                            if (index == -1) return "{\\"success\\": false, \\"message\\": \\"未找到该故事\\"}";
                            size_t count = 0;
                            auto storys = music->GetStoryLibrary(count);
                            music->SetCurrentStoryName(storys[index].story_name);
                            music->SetCurrentStoryIndex(index);
                            music->SetCurrentChapterIndex(std::max(0, chapter_idx - 1));
                            // 这里可以保持模式为顺序或者循环，外部已经设置
                            now_playing = cat + " 故事：" + storys[index].story_name + " 第" + std::to_string(chapter_idx) + "章";
                            return BuildNowPlayingPayload("{\\"call_tool\\":\\"actually.3\\"}", "读出来：将为你连续播放", now_playing);
                        } else if ((cat.empty() && story_name.empty() && chapter_idx <= 1) || goon) {
                            ESP_LOGI(TAG, "Continuing last story playback");
                            if (music->is_paused()) {
                                if (music->GetMusicOrStory_() == STORY) { music->ResumePlayback(); return true; }
                                else if (music->GetMusicOrStory_() == MUSIC) music->StopStreaming();
                            }
                            if (music->IfSavedStoryPosition()) {
                                now_playing = music->GetCurrentCategoryName() + "故事:" + music->GetCurrentStoryName() + " 第" + std::to_string(music->GetCurrentChapterIndex() + 1) + "章";
                                return BuildNowPlayingPayload("{\\"call_tool\\":\\"actually.4\\"}", "读出来：将为你播放", now_playing);
                            } else {
                                return "{\\"success\\": false, \\"message\\": \\"没有保存的故事播放记录\\"}";
                            }
                        } else {
                            // iii. 用户说某一故事的名字，或者大类+故事名
                            int index = -1;
                            if (cat.empty()) {
                                index = music->FindStoryIndexFuzzy(story_name);
                            } else {
                                index = music->FindStoryIndexInCategory(cat, story_name);
                            }
                            if (index == -1) return "{\\"success\\": false, \\"message\\": \\"未找到该故事\\"}";
                            size_t count = 0;
                            auto storys = music->GetStoryLibrary(count);
                            music->SetCurrentStoryIndex(index);
                            music->SetCurrentStoryName(storys[index].story_name);
                            music->SetCurrentCategoryName(storys[index].category);
                            music->SetCurrentChapterIndex(std::max(0, chapter_idx - 1));
                            now_playing = std::string(storys[index].category) + " 故事：" + storys[index].story_name + " 第" + std::to_string(chapter_idx) + "章";
                            return BuildNowPlayingPayload("{\\"call_tool\\":\\"actually.3\\"}", "读出来：将为你播放", now_playing);
                        }
                    } else {
                        // ==================== MUSIC 分支 ===================="""

# We need to find the begin_str and end_str and replace everything in between.
start_idx = text.find(begin_str)
end_idx = text.find(end_str)

if start_idx != -1 and end_idx != -1:
    new_text = text[:start_idx] + new_logic + text[end_idx + len(end_str):]
    with open('main/mcp_server.cc', 'w') as f:
        f.write(new_text)
    print("Replaced logic successfully!")
else:
    print("Could not find delimiters")
