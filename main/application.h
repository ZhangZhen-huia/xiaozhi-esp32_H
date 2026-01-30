#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"

#define LEDMODE_GPIO         GPIO_NUM_4
#define NORMALMODE_GPIO      GPIO_NUM_5
#define SW_LEDMODE       1
#define SW_NORMALMODE    0


#define my 0

// 超过此秒数在 idle 状态下自动进入深度睡眠（默认 1 分钟）
#ifndef IDLE_DEEP_SLEEP_SECONDS
#define IDLE_DEEP_SLEEP_SECONDS (1 * 30)
#define IDLE_DEEP_SLEEP_MUSIC_SECONDS (5 * 60)
#endif

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

enum Role{
    Player,
    Role_Xiaozhi,
    Role_XiaoMing,
};

enum DeviceFunction {
    Function_AIAssistant = 0,
    Function_Light = 1,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    void RFID_TASK();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    void AddAudioData(AudioStreamPacket&& packet);
    void SendMessage(std::string &message);

    void EnableBleWifiConfig(bool enable) { ble_wifi_config_enabled_ = enable; }
    bool IsBleWifiConfigEnabled() const { return ble_wifi_config_enabled_; }
    void EnterDeepSleep();

    bool Wifi_Offline = false;
    Role device_Role = Role_Xiaozhi;
    Role last_device_Role = Role_Xiaozhi;
    // 全局保存最近一次请求的播放时长（秒），由 music.play 设置，由 actually.* 在开始播放时读取并启动定时器
    std::atomic<int> g_requested_play_duration_sec{0};
    esp_timer_handle_t* g_play_timer_handle = nullptr;
    std::mutex g_play_timer_mutex;
    std::atomic<int64_t> g_play_timer_expire_us{0};
    std::atomic<bool> g_duration_flag = false;
    
    bool wake_word_detected_ = false;
    
    void GetSwitchState();
    int64_t GetAndClearWakeElapsedMs();
    DeviceFunction GetDeviceFunction() const { return device_function_; }
    void Resetsleep_music_ticks_(){sleep_music_ticks_ = 0;};
    void StartPlayDurationTimerIfRequested();
    bool CreateAndStartPlayTimer(uint64_t us);
    bool ExtendPlayDurationSeconds(int extra_seconds);
    void Set_PlayDuration(int duration){g_requested_play_duration_sec.store(duration);};
    void StopPlayDurationTimer();
private:
    Application();
    ~Application();

    DeviceFunction device_function_ = Function_AIAssistant;
    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t clock_Offlinetimer_handle_ = nullptr;
    int Offline_ticks_ = 0;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    volatile DeviceState device_state_last_ = kDeviceStateUnknown;

    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    int sleep_ticks_ = 0;
    int sleep_music_ticks_ = 0;
    
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;
    TaskHandle_t rfid_task_handle_ = nullptr;

    bool ble_wifi_config_enabled_ = true;
    


    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    void ShowBatteryLevel(int percent);

};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
