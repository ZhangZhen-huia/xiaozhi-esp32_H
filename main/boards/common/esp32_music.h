#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include "lvgl.h"
#include "music.h"
#include <esp_lvgl_port.h>
// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

#define MUSIC_EVENT_LOADED (1 << 0)
#define MUSIC_EVENT_COMPLETED (1 << 1)

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

class Esp32Music : public Music {
public:
    // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 1,  // 默认显示频谱
        DISPLAY_MODE_LYRICS = 0     // 显示歌词
    };

private:
    EventGroupHandle_t event_group_ = nullptr; 

    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;
    
    std::string current_cover_url_;

    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;
    
    std::thread cover_thread_;
    std::atomic<bool> is_cover_running_;

    std::atomic<DisplayMode> display_mode_;
    std::atomic<PlaybackMode> playback_mode_ = PLAYBACK_MODE_ORDER;
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
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    
    bool DownloadCover(const std::string& cover_url);
    bool ParseCover(const std::string& cover_content);
    void CoverDisplayThread();
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    int16_t* final_pcm_data_fft = nullptr;

    std::vector<MusicFileInfo> music_library_;
    mutable std::mutex music_library_mutex_;
    std::atomic<bool> music_library_scanned_;
    const std::string default_musiclist = "DefaultMusicList";
    std::vector<Playlist> playlists_;  // 使用简单的容器数组存储歌单
    std::string current_playlist_name_;
    // int play_index_ = 0;
    
    // SD卡读取线程
    void ReadFromSDCard(const std::string& file_path);
    bool StartSDCardStreaming(const std::string& file_path);

    void ScanDirectoryRecursive(const std::string& path);
    bool IsMusicFile(const std::string& file_path) const;
    MusicFileInfo ExtractMusicInfo(const std::string& file_path) const;
    std::string ExtractSongNameFromFileName(const std::string& file_name) const;
    std::vector<MusicFileInfo> SearchMusic(const std::string& keyword) const;
    std::vector<std::string> GetPlaylistNames() const;
    std::vector<MusicFileInfo> GetPlaylist(const std::string& playlist_name) const;
    
    // 播放列表辅助方法
    int FindPlaylistIndex(const std::string& name) const;

public:
    Esp32Music();
    ~Esp32Music();

    
    virtual bool Download(const std::string& song_name, const std::string& artist_name) override;
  
    virtual std::string GetDownloadResult() override;
    
    // 新增方法
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return final_pcm_data_fft; }
    virtual std::vector<std::pair<int, std::string>> GetLyrics() const override { return lyrics_; };  // 获取歌词
    // 显示模式控制方法
    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }
    virtual bool WaitForMusicLoaded()override;
    virtual void SetLoopMode(bool loop)override;
    virtual void SetRandomMode(bool random)override;
    virtual void SetOnceMode(bool once)override;
    virtual void SetOrderMode(bool order)override;
    virtual bool PlayFromSD(const std::string& file_path, const std::string& song_name = "")override;

    virtual bool ScanMusicLibrary(const std::string& music_folder)override;
    virtual size_t GetMusicCount() const override{ return music_library_.size(); }
    virtual MusicFileInfo GetMusicInfo(const std::string& file_path) const override;
    virtual std::vector<MusicFileInfo> GetMusicLibrary() const override;
    virtual bool CreatePlaylist(const std::string& playlist_name, const std::vector<std::string>& file_paths) override;
    virtual bool CreatePlaylist(const std::string& playlist_name)override;
    virtual bool PlayPlaylist(std::string& playlist_name) override;
    virtual void AddMusicToDefaultPlaylists(std::vector<std::string> default_music_files)override;
    virtual int SearchMusicIndexFromlist(std::string name, const std::string& playlist_name) const override;

    virtual void SetPlayIndex(std::string& playlist_name, int index)override;
    virtual void NextPlayIndexOrder(std::string& playlist_name)override;
    virtual void NextPlayIndexRandom(std::string& playlist_name)override;
    virtual std::string GetCurrentPlayList(void)override;
    virtual PlaybackMode GetPlaybackMode() override;
    virtual int GetLastPlayIndex(std::string& playlist_name)override;
    virtual void AddMusicToPlaylist(const std::string& playlist_name, std::vector<std::string> music_files)override;
    virtual std::string SearchMusicPathFromlist(std::string name, const std::string& playlist_name) const override;
};

#endif // ESP32_MUSIC_H