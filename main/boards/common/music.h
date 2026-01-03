#ifndef MUSIC_H
#define MUSIC_H

#include <string>
#include <vector>

struct MusicFileInfo {
    std::string file_path;
    std::string file_name;
    std::string song_name; // 规范化后的曲名（用于匹配）
    int duration = 0;
    size_t file_size = 0;

    // 新增：歌手信息
    std::string artist;       // 解析到的原始歌手名（可显示）
    std::string artist_norm;  // 规范化后用于匹配（小写、去特殊字符）
};

struct PSMusicInfo {
    char *file_path = nullptr;   // NUL 结尾 C 字符串（PSRAM 分配）
    char *file_name = nullptr;
    char *song_name = nullptr;
    char *artist = nullptr;
    char *artist_norm = nullptr;
    char *token_norm; // 新增：保留空格的小写 token-normalized 字符串（存放于 SPIRAM）
    size_t file_size = 0;
    int duration = 0;
};

struct PSStoryEntry {
    char *category = nullptr;    // PSRAM 分配的 NUL-终止字符串
    char *story_name = nullptr;  // PSRAM 分配的 NUL-终止字符串
    char **chapters = nullptr;   // PSRAM 分配的 char* 数组（每个元素也是 PSRAM 分配的字符串）
    size_t chapter_count = 0;
    std::string norm_category;   // 保留规范化用于快速比较（DRAM，轻量）
    std::string norm_story;      // 保留规范化用于快速比较（DRAM，轻量）
    char *token_norm = nullptr;  // 保留空格的小写 token-normalized 字符串（存放于 SPIRAM）
    uint32_t idx = 0;              // 故事索引编号
};
enum PlaybackMode {
    PLAYBACK_MODE_ONCE = 0,     // 播放一次
    PLAYBACK_MODE_LOOP = 1,      // 循环播放
    PLAYBACK_MODE_RANDOM = 2,     // 随机播放
    PLAYBACK_MODE_ORDER = 3       // 顺序播放
};

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    


    // 新增流式播放相关方法
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual bool IsPlaying() const =0;
    virtual void SetMusicOrStory_(int val) = 0;
    virtual bool PlayFromSD(const std::string& file_path, const std::string& song_name = "") = 0;
    virtual void SetLoopMode(bool loop) = 0;
    virtual void SetRandomMode(bool random) = 0;
    virtual void SetOnceMode(bool once) = 0;
    virtual void SetOrderMode(bool order) = 0;

    virtual bool ReturnMode(void) =0;
    virtual void SetMode(bool a) =0;
    virtual bool ScanMusicLibrary(const std::string& music_folder) = 0;
    virtual size_t GetMusicCount() const = 0;
    virtual MusicFileInfo GetMusicInfo(const std::string& file_path) const =0;

    virtual bool is_paused(void)=0;
    virtual void SetEventNextPlay(void)=0;
    virtual void PausePlayback()=0;
    virtual void ResumePlayback()=0;
    virtual bool IsActualPaused() const { return false; };
    virtual const PSMusicInfo* GetMusicLibrary(size_t &out_count) const =0;
    virtual bool CreatePlaylist(const std::string& playlist_name, const std::vector<std::string>& file_paths) = 0;
    virtual bool PlayPlaylist(const std::string& playlist_name) = 0;
    virtual int SearchMusicIndexFromlist(std::string name) const = 0;
    virtual int SearchMusicIndexFromlistByArtSong(std::string songname,std::string artist) const =0;
    virtual std::vector<int> SearchMusicIndexBySingerRand5(std::string singer) const =0;
    virtual void SetPlayIndex(const std::string& playlist_name, int index) = 0;
    virtual void NextPlayIndexOrder(std::string& playlist_name) = 0;
    virtual void NextPlayIndexRandom(std::string& playlist_name) = 0;
    virtual std::string GetCurrentPlayList(void) = 0;
    virtual PlaybackMode GetPlaybackMode()  = 0;
    virtual void SetCurrentPlayList(const std::string& playlist_name) = 0;
    virtual const std::string GetDefaultList() const =0;
    virtual std::string SearchMusicFromlistByIndex(std::string list) const =0;
    virtual void ScanAndLoadMusic() = 0;
    virtual void LoadPlaybackPosition() = 0;
    virtual void SavePlaybackPosition() = 0;
    virtual bool ResumeSavedPlayback() = 0;
    virtual bool IfSavedMusicPosition()  = 0;
    virtual std::string GetCurrentSongName() = 0;
    virtual void UpdateMusicRecordList(const std::string& artist, const std::string& song_name) = 0;
    virtual void EnableRecord(bool x,bool MusicOrStory)=0;
    virtual bool GetIfRecordEnabled(bool MusicOrStory) const =0;
    virtual bool IfNodeIsEnd(bool MusicOrStory) const =0;
    virtual int NextNodeIndex(bool MusicOrStory) =0;
    virtual int LastNodeIndex(bool MusicOrStory) =0;
    virtual bool ScanStoryLibrary(const std::string& story_folder) = 0;
    virtual std::vector<std::string> GetStoryCategories() const = 0;
    virtual std::vector<std::string> GetStoriesInCategory(const std::string& category) const = 0;
    virtual std::vector<std::string> GetChaptersForStory(const std::string& category, const std::string& story_name) const = 0;
    // 从已索引中选择并播放某类别/故事/章节（chapter_index 可选，默认 0）
    virtual bool SelectStoryAndPlay() = 0;
    virtual bool IfSavedStoryPosition() = 0;
    virtual void SaveStoryPlaybackPosition() = 0;
    virtual void LoadStoryPlaybackPosition() = 0;
    virtual bool ResumeSavedStoryPlayback() = 0;
    virtual std::string GetCurrentStoryName() = 0;
    virtual std::string GetCurrentCategoryName() =0;
    virtual int GetCurrentChapterIndex() =0;
    virtual const PSStoryEntry* GetStoryLibrary(size_t &out_count) const =0;
    virtual std::string GetCurrentChapterName() =0;
    virtual void ScanAndLoadStory()=0;
    virtual int GetMusicOrStory_() const=0;
    virtual bool NextChapterInStory(const std::string& category, const std::string& story_name) = 0;

    virtual void SetCurrentCategoryName(const std::string& category) = 0;
    virtual void SetCurrentStoryName(const std::string& story) =0;
    virtual void SetCurrentChapterIndex(int index) = 0;
    virtual bool NextStoryInCategory(const std::string& category) = 0;
    virtual void SetCurrentStoryChapter(int index) = 0;
    virtual size_t FindStoryIndexInCategory(const std::string& category, const std::string& story_name) const = 0;
    virtual size_t FindStoryIndexFuzzy(const std::string& story_name) const = 0;
    virtual void UpdateStoryRecordList(const std::string& category, const std::string& story, const std::string& chapter) = 0;

};

#endif // MUSIC_H 