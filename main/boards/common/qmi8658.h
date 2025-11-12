#pragma once

#include "i2c_device.h"
#include <stdint.h>
#include "math.h"
#include "display.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"

#define  QMI8658_SENSOR_ADDR       0x6A   // QMI8658 I2C地址

// QMI8658寄存器地址
enum qmi8658_reg
{
    QMI8658_WHO_AM_I,
    QMI8658_REVISION_ID,
    QMI8658_CTRL1,
    QMI8658_CTRL2,
    QMI8658_CTRL3,
    QMI8658_CTRL4,
    QMI8658_CTRL5,
    QMI8658_CTRL6,
    QMI8658_CTRL7,
    QMI8658_CTRL8,
    QMI8658_CTRL9,
    QMI8658_CATL1_L,
    QMI8658_CATL1_H,
    QMI8658_CATL2_L,
    QMI8658_CATL2_H,
    QMI8658_CATL3_L,
    QMI8658_CATL3_H,
    QMI8658_CATL4_L,
    QMI8658_CATL4_H,
    QMI8658_FIFO_WTM_TH,
    QMI8658_FIFO_CTRL,
    QMI8658_FIFO_SMPL_CNT,
    QMI8658_FIFO_STATUS,
    QMI8658_FIFO_DATA,
    QMI8658_STATUSINT = 45,
    QMI8658_STATUS0,
    QMI8658_STATUS1,
    QMI8658_TIMESTAMP_LOW,
    QMI8658_TIMESTAMP_MID,
    QMI8658_TIMESTAMP_HIGH,
    QMI8658_TEMP_L,
    QMI8658_TEMP_H,
    QMI8658_AX_L,
    QMI8658_AX_H,
    QMI8658_AY_L,
    QMI8658_AY_H,
    QMI8658_AZ_L,
    QMI8658_AZ_H,
    QMI8658_GX_L,
    QMI8658_GX_H,
    QMI8658_GY_L,
    QMI8658_GY_H,
    QMI8658_GZ_L,
    QMI8658_GZ_H,
    QMI8658_COD_STATUS = 70,
    QMI8658_dQW_L = 73,
    QMI8658_dQW_H,
    QMI8658_dQX_L,
    QMI8658_dQX_H,
    QMI8658_dQY_L,
    QMI8658_dQY_H,
    QMI8658_dQZ_L,
    QMI8658_dQZ_H,
    QMI8658_dVX_L,
    QMI8658_dVX_H,
    QMI8658_dVY_L,
    QMI8658_dVY_H,
    QMI8658_dVZ_L,
    QMI8658_dVZ_H,
    QMI8658_TAP_STATUS = 89,
    QMI8658_STEP_CNT_LOW,
    QMI8658_STEP_CNT_MIDL,
    QMI8658_STEP_CNT_HIGH,
    QMI8658_RESET = 96
};

    // 倾角结构体
typedef struct{
    int16_t acc_x;
    int16_t acc_y;
    int16_t acc_z;
    int16_t gyr_x;
    int16_t gyr_y;
    int16_t gyr_z;
    float AngleX;
    float AngleY;
    float AngleZ;
}t_sQMI8658;


class QMI8658 :public I2cDevice {
    private:

    
    uint8_t IMU_UUID = 0;
    public:
    void qmi8658_init(void)
    {
        IMU_UUID = ReadReg(0x0);
        while(IMU_UUID != 0x05)
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            IMU_UUID = ReadReg(0x0);
        }
        // ESP_LOGI(TAG, "QMI8658 OK!");  // 打印信息
        WriteReg(QMI8658_RESET, 0xb0);  // 复位  
        vTaskDelay(10 / portTICK_PERIOD_MS);  // 延时10ms
        WriteReg(QMI8658_CTRL1, 0x40); // CTRL1 设置地址自动增加
        WriteReg(QMI8658_CTRL7, 0x03); // CTRL7 允许加速度和陀螺仪
        WriteReg(QMI8658_CTRL2, 0x95); // CTRL2 设置ACC 4g 250Hz
        WriteReg(QMI8658_CTRL3, 0xd5); // CTRL3 设置GRY 512dps 250Hz 
    }

    QMI8658(i2c_master_bus_handle_t i2c_bus, uint8_t addr) :I2cDevice(i2c_bus, addr)
    {
        qmi8658_init();
    }
    void qmi8658_Read_AccAndGry(t_sQMI8658 *imu);
    void qmi8658_fetch_angleFromAcc(t_sQMI8658 *imu);
    void qmi8658_application(t_sQMI8658 *imu);
};
