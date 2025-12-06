#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <map>
#include "lvgl.h"
#include "music.h"
#include <esp_lvgl_port.h>
// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}



#define STORY 1
#define MUSIC 0

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

// 播放列表结构
struct Playlist {
    std::string name;
    std::vector<std::string> file_paths;
    int play_index = 0;
    int last_play_index = 0;
    Playlist(const std::string& n = "") : name(n) {}
};

// 新增：描述歌曲元数据（歌手 + 歌名）及规范化字段
struct SongMeta {
    std::string artist;             // 原始解析到的歌手（可能包含空格/特殊字符）
    std::string title;              // 原始解析到的曲名
    std::string norm_artist;        // 规范化后用于匹配（小写、去掉非字母数字）
    std::string norm_title;         // 规范化后用于匹配（小写、去掉非字母数字）
};

// 描述一个故事（属于某类别），包含若干章节（文件路径）
struct StoryEntry {
    std::string category;
    std::string story;
    std::vector<std::string> chapters; // 章节的完整文件路径（按文件名排序）
    std::string norm_category;
    std::string norm_story;
};
struct PSStoryEntry {
    char *category = nullptr;    // PSRAM 分配的 NUL-终止字符串
    char *story_name = nullptr;  // PSRAM 分配的 NUL-终止字符串
    char **chapters = nullptr;   // PSRAM 分配的 char* 数组（每个元素也是 PSRAM 分配的字符串）
    size_t chapter_count = 0;
    std::string norm_category;   // 保留规范化用于快速比较（DRAM，轻量）
    std::string norm_story;      // 保留规范化用于快速比较（DRAM，轻量）
};

class Esp32Music : public Music {
public:
    // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 1,  // 默认显示频谱
        DISPLAY_MODE_LYRICS = 0     // 显示歌词
    };

private:
    std::string last_downloaded_data_;
    std::string current_song_name_;

    
    std::atomic<PlaybackMode> MusicPlayback_mode_ = PLAYBACK_MODE_ORDER;
    std::atomic<PlaybackMode> StoryPlayback_mode_ = PLAYBACK_MODE_ORDER;

    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB缓冲区
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;   // 32KB最小播放缓冲
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    // 私有方法
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    int16_t* final_pcm_data_fft = nullptr;

    PSMusicInfo *ps_music_library_ = nullptr; // 分配在 PSRAM（heap_caps_malloc）的音乐库数组
    int play_index_ = 0;
    int last_play_index_ = 0;
    size_t ps_music_count_ = 0;                // 音乐库中音乐
    size_t ps_music_capacity_ = 0;
    

    mutable std::mutex music_library_mutex_;
    std::atomic<bool> music_library_scanned_;
    const std::string default_musiclist_ = "DefaultMusicList";
    Playlist playlist_;  // 使用简单的容器数组存储歌单
    std::string current_playlist_name_;
    std::atomic<int> MusicOrStory_;
    // SD卡读取线程
    void ReadFromSDCard(const std::string& file_path);
    bool StartSDCardStreaming(const std::string& file_path);

    void ScanDirectoryRecursive(const std::string& path);
    bool IsMusicFile(const std::string& file_path) const;
    MusicFileInfo ExtractMusicInfo(const std::string& file_path) const;
    bool ps_add_music_info_locked(const MusicFileInfo &info);
    bool ps_add_story_locked(const StoryEntry &e);
    void free_ps_music_library_locked();
    void free_ps_story_index_locked();
    
    
    char* ps_strdup(const std::string &s);
    void ps_free_str(char *p);


    PSStoryEntry *ps_story_index_ = nullptr; // PSRAM 分配的数组
    size_t ps_story_count_ = 0;
    size_t ps_story_capacity_ = 0;
    mutable std::mutex story_index_mutex_;
    std::string current_story_name_;
    std::string current_category_name_;
    int current_chapter_index_ = 0;

    // 保存的故事断点（持久化/恢复用）
    std::string saved_story_category_;
    std::string saved_story_name_;
    int saved_chapter_index_ = -1;
    uint64_t saved_chapter_file_offset_ = 0; // 字节偏移
    int saved_chapter_ms_ = 0; // 可选的时间位置（ms）
    bool has_saved_story_position_ = false;

    // 当前播放文件指针与偏移（用于记录并恢复）
    FILE* current_play_file_ = nullptr;
    size_t current_play_file_offset_ = 0;
    std::mutex current_play_file_mutex_;

    // 请求在 ReadFromSDCard 时从该偏移开始读取（由 PlayFromSD(..., start_offset) 设置）
    size_t start_play_offset_ = 0;

    // NVS 上保存的音乐断点
    int saved_play_index_ = -1;
    int64_t saved_play_ms_ = 0;
    size_t saved_file_offset_ = 0;
    std::string saved_file_path_;
    bool has_saved_MusicPosition_ = false;


public:
    Esp32Music();
    ~Esp32Music();

    virtual void SetMusicOrStory_(int val) override{
        MusicOrStory_ = val;
    }
    virtual int GetMusicOrStory_() const override {
        return MusicOrStory_;
    }
    // 新增方法
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }




    //播放音乐相关方法
    virtual void SetLoopMode(bool loop)override;
    virtual void SetRandomMode(bool random)override;
    virtual void SetOnceMode(bool once)override;
    virtual void SetOrderMode(bool order)override;
    virtual bool PlayFromSD(const std::string& file_path, const std::string& song_name = "")override;
    virtual bool PlayFromSD(const std::string& file_path, const std::string& song_name, size_t start_offset);

    virtual bool ScanMusicLibrary(const std::string& music_folder)override;
    virtual size_t GetMusicCount() const override{ return ps_music_count_; };
    virtual MusicFileInfo GetMusicInfo(const std::string& file_path) const override;
    virtual const PSMusicInfo* GetMusicLibrary(size_t &out_count) const override;
    virtual bool CreatePlaylist(const std::string& playlist_name, const std::vector<std::string>& file_paths) override;

    virtual bool PlayPlaylist(std::string& playlist_name) override;
    virtual int SearchMusicIndexFromlist(std::string name) const override;
    virtual const std::string GetDefaultList() const override { return default_musiclist_; }
    virtual std::string GetCurrentSongName()override;
    virtual void SetPlayIndex(std::string& playlist_name, int index)override;
    virtual void NextPlayIndexOrder(std::string& playlist_name)override;
    virtual void NextPlayIndexRandom(std::string& playlist_name)override;
    virtual std::string GetCurrentPlayList(void)override;
    virtual PlaybackMode GetPlaybackMode() override;
    virtual void SetCurrentPlayList(const std::string& playlist_name)override;
    virtual void ScanAndLoadMusic()override;
    virtual void LoadPlaybackPosition()override;
    virtual void SavePlaybackPosition()override;
    virtual bool ResumeSavedPlayback()override;
    virtual std::string SearchMusicFromlistByIndex(std::string list) const override;
    virtual bool IfSavedMusicPosition() override{ return has_saved_MusicPosition_; };
    virtual std::vector<std::string> SearchSinger(std::string singer) const override;


    // 故事播放相关接口
    virtual bool ScanStoryLibrary(const std::string& story_folder) override;
    virtual bool IfSavedStoryPosition() override { return has_saved_story_position_; };
    virtual std::string GetCurrentStoryName() override{ return current_story_name_; };
    virtual std::string GetCurrentCategoryName() override{ return current_category_name_; };
    virtual int GetCurrentChapterIndex() override{ return current_chapter_index_; };

    virtual void SetCurrentCategoryName(const std::string& category)override;

    virtual void SetCurrentStoryName(const std::string& story)override;

    virtual void SetCurrentChapterIndex(int index)override;
    virtual std::vector<std::string> GetStoryCategories() const;
    virtual std::vector<std::string> GetStoriesInCategory(const std::string& category) const;
    virtual std::vector<std::string> GetChaptersForStory(const std::string& category, const std::string& story_name) const;
    // 从已索引中选择并播放某类别/故事/章节（chapter_index 可选，默认 0）
    virtual bool SelectStoryAndPlay()override;

    // 故事断点保存/加载/恢复接口（类似音乐的断点）
    virtual void SaveStoryPlaybackPosition()override;
    virtual void LoadStoryPlaybackPosition()override;
    virtual bool ResumeSavedStoryPlayback()override;
    virtual void ScanAndLoadStory()override;
    virtual bool NextChapterInStory(const std::string& category, const std::string& story_name) override;
    virtual bool NextStoryInCategory(const std::string& category) override;
};


// 全局辅助函数：从文件名或输入中解析出 SongMeta
SongMeta ParseSongMeta(const std::string& filename);
std::string NormalizeForSearch(std::string s) ;
#endif // ESP32_MUSIC_H