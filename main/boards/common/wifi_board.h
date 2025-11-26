#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"


class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    
    virtual std::string GetBoardJson() override;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t clock_timer_OnConnecthandle_ = nullptr;

public:
    WifiBoard();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    virtual void EnterWifiConfigMode() override;
};

#endif // WIFI_BOARD_H
