#include "led.h"
#include "application.h"

void Led::Blink(int ms_on, int ms_off) {
    if (pin_ == GPIO_NUM_NC) return;
    gpio_set_level(pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(ms_on));
    gpio_set_level(pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(ms_off));
}

void Led::Set(bool on) {
    if (pin_ != GPIO_NUM_NC) gpio_set_level(pin_, on ? 1 : 0);
}

void Led::OnStateChanged() {
    auto &app = Application::GetInstance();
    switch (app.GetDeviceState()) {
        case kDeviceStateListening:
            Set(true);
            break;
        case kDeviceStateSpeaking:
            Set(true);
            break;
        default:
            Set(false);
            break;  
    }
}