#pragma once

#include "driver/gpio.h"
#include <stdio.h>
#include <esp_log.h>

class Led {
public:
    explicit Led(gpio_num_t pin = GPIO_NUM_NC) : pin_(pin) {
        if (pin_ != GPIO_NUM_NC) {
            gpio_reset_pin(pin_);
            gpio_set_direction(pin_, GPIO_MODE_OUTPUT);
            gpio_set_level(pin_, 0);
        }
    }
    // 禁用拷贝
    Led(const Led&) = delete;
    Led& operator=(const Led&) = delete;

    void Set(bool on);
    void Blink(int ms_on, int ms_off);
    void OnStateChanged();
private:
    gpio_num_t pin_;
};