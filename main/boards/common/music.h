#ifndef MUSIC_H
#define MUSIC_H

#include <string>

struct MusicFileInfo {
    std::string file_path;
    std::string file_name;
    std::string song_name;
    std::string artist;
    std::string album;
    size_t file_size;
    int duration; // 秒
    
    MusicFileInfo() : file_size(0), duration(0) {}
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

    virtual bool ScanMusicLibrary(const std::string& music_folder) = 0;
    virtual size_t GetMusicCount() const = 0;
    virtual MusicFileInfo GetMusicInfo(const std::string& file_path) const = 0;
    virtual std::vector<MusicFileInfo> GetMusicLibrary() const = 0;
    virtual bool CreatePlaylist(const std::string& playlist_name, const std::vector<std::string>& file_paths) = 0;
    virtual bool PlayPlaylist(const std::string& playlist_name) = 0;
    virtual void AddMusicToDefaultPlaylists(std::vector<std::string> default_music_files) = 0;
};

#endif // MUSIC_H 