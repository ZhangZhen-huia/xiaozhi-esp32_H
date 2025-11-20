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

enum PlaybackMode {
    PLAYBACK_MODE_ONCE = 0,     // 播放一次
    PLAYBACK_MODE_LOOP = 1,      // 循环播放
    PLAYBACK_MODE_RANDOM = 2,     // 随机播放
    PLAYBACK_MODE_ORDER = 3       // 顺序播放
};

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual std::string GetDownloadResult() = 0;
    
    // 新增流式播放相关方法
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual int16_t* GetAudioData() = 0;
    virtual std::vector<std::pair<int, std::string>> GetLyrics() const = 0;  // 获取歌词
    virtual bool WaitForMusicLoaded() = 0;
    virtual bool PlayFromSD(const std::string& file_path, const std::string& song_name = "") = 0;
    virtual void SetLoopMode(bool loop) = 0;
    virtual void SetRandomMode(bool random) = 0;
    virtual void SetOnceMode(bool once) = 0;
    virtual void SetOrderMode(bool order) = 0;
    virtual bool ScanMusicLibrary(const std::string& music_folder) = 0;
    virtual size_t GetMusicCount() const = 0;
    virtual MusicFileInfo GetMusicInfo(const std::string& file_path) const = 0;
    virtual std::vector<MusicFileInfo> GetMusicLibrary() const = 0;
    virtual bool CreatePlaylist(const std::string& playlist_name, const std::vector<std::string>& file_paths) = 0;
    virtual bool PlayPlaylist(std::string& playlist_name) = 0;
    virtual void AddMusicToDefaultPlaylists(std::vector<std::string> default_music_files) = 0;
    virtual int SearchMusicIndexFromlist(std::string name, const std::string& playlist_name) const = 0;
    virtual void SetPlayIndex(std::string& playlist_name, int index) = 0;
    virtual void NextPlayIndexOrder(std::string& playlist_name) = 0;
    virtual void NextPlayIndexRandom(std::string& playlist_name) = 0;
    virtual std::string GetCurrentPlayList(void) = 0;
    virtual PlaybackMode GetPlaybackMode()  = 0;
    virtual int GetLastPlayIndex(std::string& playlist_name) = 0;
    virtual bool CreatePlaylist(const std::string& playlist_name) = 0;
    virtual std::string SearchMusicPathFromlist(std::string name, const std::string& playlist_name) const = 0;
    virtual void SetCurrentPlayList(const std::string& playlist_name) = 0;
    virtual std::string ExtractSongNameFromFileName(const std::string& file_name) const = 0;
    virtual int FindPlaylistIndex(const std::string& name) const = 0;
    virtual void SavePlaylistsToNVS() = 0;
    virtual bool LoadPlaylistsFromNVS() = 0;
    virtual void InitializeDefaultPlaylists() = 0;
    virtual void LoadPlaybackPosition() = 0;
    virtual void SavePlaybackPosition() = 0;
    virtual bool ResumeSavedPlayback() = 0;
    virtual bool IfSavedPosition()  = 0;
    virtual std::vector<std::string> SearchSingerFromlist(std::string singer, const std::string& playlist_name) const =0;
    virtual std::string GetCurrentSongName() = 0;
    virtual bool DeletePlaylistFromNVS(const std::string& playlist_name) = 0;
    virtual bool DeleteAllPlaylistsExceptDefault() = 0;


};

#endif // MUSIC_H 