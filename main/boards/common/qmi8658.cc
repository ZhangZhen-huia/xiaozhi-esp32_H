#include "qmi8658.h"
#include "application.h"
static const char *TAG = "QMI8658";


// 读取加速度和陀螺仪寄存器值
void QMI8658:: qmi8658_Read_AccAndGry(t_sQMI8658 *imu)
{
    uint8_t status, data_ready=0;
    int16_t buf[6];
    status = ReadReg(QMI8658_STATUS0); // 读状态寄存器 
    if (status & 0x03) // 判断加速度和陀螺仪数据是否可读
        data_ready = 1;
    if (data_ready == 1){  // 如果数据可读
        data_ready = 0;
        ReadRegs(QMI8658_AX_L, (uint8_t *)buf, 12); // 读加速度和陀螺仪值
        imu->acc_x = buf[0];
        imu->acc_y = buf[1];
        imu->acc_z = buf[2];
        imu->gyr_x = buf[3];
        imu->gyr_y = buf[4];
        imu->gyr_z = buf[5];
    }
    else
    {
        ESP_LOGE(TAG, "QMI8658 Reinit!");  // 打印信息
        qmi8658_init(); // 初始化
    }
}
// 获取XYZ轴的倾角值
void QMI8658:: qmi8658_fetch_angleFromAcc(t_sQMI8658 *imu)
{
    float temp;
    qmi8658_Read_AccAndGry(imu); // 读取加速度和陀螺仪的寄存器值
    // 根据寄存器值 计算倾角值 并把弧度转换成角度
    temp = (float)imu->acc_x / sqrt( ((float)imu->acc_y * (float)imu->acc_y + (float)imu->acc_z * (float)imu->acc_z) );
    imu->AngleX = atan(temp)*57.29578f; // 180/π=57.29578
    temp = (float)imu->acc_y / sqrt( ((float)imu->acc_x * (float)imu->acc_x + (float)imu->acc_z * (float)imu->acc_z) );
    imu->AngleY = atan(temp)*57.29578f; // 180/π=57.29578
    temp = sqrt( ((float)imu->acc_x * (float)imu->acc_x + (float)imu->acc_y * (float)imu->acc_y) ) / (float)imu->acc_z;
    imu->AngleZ = atan(temp)*57.29578f; // 180/π=57.29578
}

void QMI8658:: qmi8658_application(t_sQMI8658 *imu)
{
    float angle_x = 0, angle_y = 0, angle_z = 0;
    qmi8658_fetch_angleFromAcc(imu);

    auto& app = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    if(imu->AngleY<=-55)
    {
        if(display->current_screen_ == display->main_screen_) {
            if(display->offlinemusic_screen_ == nullptr) {
                display->OfflineMusicUI();
            }
            lv_obj_add_flag(display->main_screen_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(display->offlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
            display->current_screen_ = display->offlinemusic_screen_;
            display->OfflineMusicUI_Recover();
        }
    }
    if(imu->AngleY>=55)
    {
        if(display->current_screen_ == display->offlinemusic_screen_) {
            display->OfflineMusicUI_Deinit();
            display->current_screen_ = display->main_screen_;
            lv_obj_add_flag(display->offlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(display->main_screen_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}