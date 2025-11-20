#include "lcd_display.h"
 #include <esp_log.h>
#include <esp_lvgl_port.h>
#include "assets/lang_config.h"
#include <cstring>
#include "settings.h"
#include "application.h"
#include "board.h"
#include "esp_jpeg_dec.h"
#include "boards/common/esp32_music.h"
esp_err_t process_jpeg(uint8_t *jpeg_data, size_t jpeg_size, uint8_t **rgb565_data, size_t *rgb565_size, size_t *width, size_t *height);


#define TAG "LcdDisplay_music"

// LV_IMG_DECLARE(img3);
int32_t g_img_angle;   // 0.1° 单位
lv_timer_t * hide_timer;     /* 一次性定时器 */
lv_obj_t *img_cover;          /* 封面 */
lv_obj_t *label_title;
lv_obj_t *slider_progress;
lv_obj_t *slider_vol;
lv_obj_t *btn_play;           /* 底部播放/暂停按钮 */
lv_obj_t *list_song;
lv_obj_t *list_screen;
lv_obj_t *hidden_btn;
lv_obj_t *song_list_btn;
bool is_playing = false;
lv_anim_t anim_cover;
lv_obj_t *time_label;
static bool list_visible = false;//当前状态 


static void rotate_cover(void *img, int32_t angle) {
    lv_image_set_rotation((lv_obj_t *)img, angle);     // 9.x 单位仍是 0.1°
    g_img_angle = angle;
}

//隐藏滑条的重启函数
static void restart_hide_timer(void)
{
    if(hide_timer) {
        lv_timer_del(hide_timer);
    }
    //新建一次性 2000 ms 定时器
    hide_timer = lv_timer_create([](lv_timer_t * t){
        lv_obj_add_flag(slider_vol, LV_OBJ_FLAG_HIDDEN);
        lv_timer_del(hide_timer);       // 用完即毁
        hide_timer = NULL;
    }, 2000, NULL);
    lv_timer_set_repeat_count(hide_timer, 1);   //只跑一次
}


static void list_show(bool show)
{
    int32_t start, end;
    if(show) {
        start = 160;
        end   = 10;
        lv_obj_clear_flag(list_song, LV_OBJ_FLAG_HIDDEN);
    } else {
        start = 10;
        end   = 160;
    }
    list_visible = show;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, list_song);
    lv_anim_set_exec_cb(&a, [](void * obj, int32_t x){
        lv_obj_set_x((lv_obj_t *)obj, x);
    });
    lv_anim_set_values(&a, start, end);
    lv_anim_set_time(&a, 300);                     //300 ms
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, [](lv_anim_t * a){
        if(!list_visible) {
        lv_obj_add_flag(list_song, LV_OBJ_FLAG_HIDDEN);//动画结束再隐藏
    }
    });
    lv_anim_start(&a);
}


static void btn_playlist_cb(lv_event_t * e)
{
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if(btn == song_list_btn)
            list_show(!list_visible);
        else if(btn == hidden_btn)
            {
                if(!lv_obj_has_flag(list_song, LV_OBJ_FLAG_HIDDEN))
                    list_show(false);
            }
    }
}

void LcdDisplay ::OfflineMusicUI_Deinit()
{
    DisplayLockGuard lock(this);
    lv_anim_delete(img_cover, rotate_cover);
}
void LcdDisplay ::OfflineMusicUI_Recover()
{
    DisplayLockGuard lock(this);
    if(is_playing){
        lv_anim_init(&anim_cover);
        lv_anim_set_var(&anim_cover, img_cover);
        lv_anim_set_exec_cb(&anim_cover, rotate_cover);
        lv_anim_set_values(&anim_cover, g_img_angle, g_img_angle + 3600); // 3600 = 360.0°
        lv_anim_set_time(&anim_cover, 30000);    // 30 秒转一圈
        lv_anim_set_path_cb(&anim_cover, lv_anim_path_linear);
        lv_anim_set_repeat_count(&anim_cover, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim_cover);
    }
} 

esp_err_t process_jpeg(uint8_t *jpeg_data, size_t jpeg_size, uint8_t **rgb565_data, size_t *rgb565_size, size_t *width, size_t *height);

void LcdDisplay::OfflineMusicUI() 
{
    DisplayLockGuard lock(this);
    
    lv_theme_t *th = lv_theme_default_init(NULL, lv_palette_main(LV_PALETTE_RED),
                                           lv_palette_main(LV_PALETTE_GREY),
                                           true, LV_FONT_DEFAULT);
    lv_disp_set_theme(NULL, th);
    offlinemusic_screen_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(offlinemusic_screen_, 320, 240);
    lv_obj_set_style_border_width(offlinemusic_screen_, 0, 0);
    lv_obj_clear_flag(offlinemusic_screen_, LV_OBJ_FLAG_SCROLLABLE);
    current_screen_ = offlinemusic_screen_;
    
    /* 顶部标题 */
    label_title = lv_label_create(offlinemusic_screen_);
    lv_label_set_text(label_title, "Music LVGL Demo");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 40, 5);

    //图片容器
    lv_obj_t * circle = lv_obj_create(offlinemusic_screen_);
    lv_obj_set_size(circle, 115, 115);                   /* 直径 115 px */
    lv_obj_align(circle, LV_ALIGN_LEFT_MID, 0, -40);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);/* 变成正圆 */
    lv_obj_set_style_clip_corner(circle, true, 0);       /* 裁剪超出部分 */
    lv_obj_set_style_border_width(circle, 5, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);              /* 去掉内边距 */
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    img_cover = lv_img_create(circle);
    // lv_image_set_src(img_cover, &img3);  // 使用下载的封面
    lv_obj_center(img_cover);
    lv_obj_invalidate(img_cover);

    hidden_btn = lv_btn_create(offlinemusic_screen_);
    lv_obj_set_size(hidden_btn, 170,240);
    lv_obj_set_style_bg_opa(hidden_btn,0,LV_PART_MAIN);
    lv_obj_align(hidden_btn,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_add_event_cb(hidden_btn,btn_playlist_cb,LV_EVENT_CLICKED,NULL);

    /* 进度条 */
    slider_progress = lv_slider_create(offlinemusic_screen_);
    lv_obj_set_size(slider_progress, 250, 5);
    lv_obj_align_to(slider_progress, offlinemusic_screen_, LV_ALIGN_BOTTOM_LEFT, 5, -60);
    lv_slider_set_range(slider_progress, 0, 100);



    lv_obj_t *volume_btn = lv_btn_create(offlinemusic_screen_);
    lv_obj_set_size(volume_btn,40,40);
    lv_obj_align_to(volume_btn,slider_progress,LV_ALIGN_OUT_RIGHT_MID,10,0);
    lv_obj_set_style_bg_opa(volume_btn,0,LV_PART_MAIN);
    lv_obj_add_event_cb(volume_btn, [](lv_event_t *e)
    {
        lv_event_code_t code = lv_event_get_code(e);

        if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
            lv_obj_clear_flag(slider_vol, LV_OBJ_FLAG_HIDDEN);
        }
        else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            restart_hide_timer();       
        }
    }, LV_EVENT_ALL, NULL);

    lv_obj_t *vollum_label = lv_label_create(volume_btn);
    lv_obj_align(vollum_label,LV_ALIGN_CENTER,0,0);
    lv_label_set_text(vollum_label,LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vollum_label, &lv_font_montserrat_14, 0);


    slider_vol = lv_slider_create(offlinemusic_screen_);
    lv_slider_set_range(slider_vol, 0, 100);
    lv_slider_set_value(slider_vol, 60, LV_ANIM_OFF);
    lv_obj_set_size(slider_vol, 5, 100);
    lv_obj_align_to(slider_vol, volume_btn, LV_ALIGN_OUT_TOP_MID, 0, 0);
    lv_obj_add_flag(slider_vol, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(slider_vol, 5, LV_PART_KNOB);   //缩小滑块大小
    lv_obj_add_event_cb(slider_vol, [](lv_event_t *e){
        //只要值在动，就刷新计时
        if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            restart_hide_timer();
        }

        //原来的音量处理 
        int32_t v = lv_slider_get_value(slider_vol);
        ESP_LOGI(TAG, "Volume: %ld", v);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    /* 底部控制栏容器 */
    lv_obj_t *bar = lv_obj_create(offlinemusic_screen_);
    lv_obj_set_size(bar, 320, 50);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);   /* 不要滚动条 */

    /* 上一首 / 播放 / 下一首 */
    lv_obj_t *btn_prev = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(btn_prev,0,LV_PART_MAIN);
    lv_obj_t *label_prev = lv_label_create(btn_prev);
    lv_label_set_text(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_14, 0);

    btn_play = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(btn_play,0,LV_PART_MAIN);
    lv_obj_t *label_play = lv_label_create(btn_play);
    lv_label_set_text(label_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(label_play, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn_play, [](lv_event_t *e)
    {
        is_playing = !is_playing;
        lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_label_set_text(label, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if(is_playing){
            lv_anim_init(&anim_cover);
            lv_anim_set_var(&anim_cover, img_cover);
            lv_anim_set_exec_cb(&anim_cover, rotate_cover);
            // lv_anim_set_values(&anim_cover, 0, 3600);   // 0→360°
            lv_anim_set_values(&anim_cover, g_img_angle, g_img_angle + 3600); // 从当前值再走360°
            lv_anim_set_time(&anim_cover, 10000);       // 10 s
            lv_anim_set_repeat_count(&anim_cover, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&anim_cover);
        }
        else{
            lv_anim_delete(img_cover, rotate_cover);    // 9.x 用 delete
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_t *btn_next = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(btn_next,0,LV_PART_MAIN);
    lv_obj_t *label_next = lv_label_create(btn_next);
    lv_label_set_text(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &lv_font_montserrat_14, 0);

    //播放列表按钮
    song_list_btn = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(song_list_btn,0,LV_PART_MAIN);
    lv_obj_add_event_cb(song_list_btn, btn_playlist_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *song_list_label = lv_label_create(song_list_btn);
    lv_label_set_text(song_list_label, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(song_list_label, &lv_font_montserrat_14, 0);


    //时间标签
    time_label = lv_label_create(offlinemusic_screen_);
    lv_label_set_text(time_label, "00:00 / 03:45");
    lv_obj_align_to(time_label, slider_progress, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);



    /* 播放列表 */
    list_song = lv_list_create(offlinemusic_screen_);
    lv_obj_set_size(list_song, 150, 200);
    lv_obj_align(list_song, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(list_song, LV_OBJ_FLAG_HIDDEN);   /* 初始隐藏 */


    /* 添加一些歌曲 */
    for (int i = 0; i < 5; i++) {
        lv_obj_t *btn = lv_list_add_button(list_song, NULL, "");
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text_fmt(lab, "Song %02d", i + 1);
        lv_obj_add_event_cb(btn, [](lv_event_t *e)
        {
            uint32_t idx = lv_obj_get_index((lv_obj_t *)lv_event_get_target(e));   /* 拿到序号 */
            lv_label_set_text_fmt(label_title, "正在播放：Song %02ld", idx + 1);
            //切歌后复位进度
            lv_slider_set_value(slider_progress, 0, LV_ANIM_ON);
        }, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_add_flag(offlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
}



void LcdDisplay::OfflineUpdatePlayTime(int64_t current_time_ms)
{
    DisplayLockGuard lock(this);
    if(current_screen_ != offlinemusic_screen_)
        return;

    int total_seconds = current_time_ms / 1000;
    int minutes = total_seconds / 60;
    int seconds = total_seconds % 60;

    // 假设总时长为 3 分 45 秒
    int total_duration_seconds = 3 * 60 + 45;
    int progress = (total_seconds * 100) / total_duration_seconds;
    if (progress > 100) progress = 100;

    lv_slider_set_value(slider_progress, progress, LV_ANIM_ON);
    lv_label_set_text_fmt(time_label, "%02d:%02d / 03:45", minutes, seconds);
}

/**
 * @brief 解码JPEG数据为RGB565格式
 *
 * @param jpeg_data 输入的JPEG数据
 * @param jpeg_size JPEG数据大小
 * @param rgb565_data 输出的RGB565数据指针（函数内分配内存，调用者负责释放）
 * @param width 输出的图像宽度
 * @param height 输出的图像高度
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t process_jpeg(uint8_t *jpeg_data, size_t jpeg_size, uint8_t **rgb565_data, size_t *rgb565_size, size_t *width, size_t *height)
{
    // 调试输出
    // ESP_LOGI("JPEG", "Received JPEG: %d bytes [%02X %02X ... %02X %02X]",
    //  jpeg_size, jpeg_data[0], jpeg_data[1], jpeg_data[jpeg_size - 2], jpeg_data[jpeg_size - 1]);

    // 检查JPEG头标记
    // 判断jpeg_size是否小于4，或者jpeg_data的第一个字节是否为0xFF，第二个字节是否为0xD8
    if (jpeg_size < 4 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8)
    {
        // 如果不满足上述条件，则输出错误信息
        ESP_LOGE("JPEG", "Invalid JPEG header");
        // 返回
        return ESP_ERR_INVALID_ARG;
    }

    // 初始化JPEG解码器
    // 声明一个jpeg解码器句柄
    jpeg_dec_handle_t jpeg_dec;
    // 声明一个jpeg解码器配置结构体，并初始化为默认值
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    // 设置输出格式为RGB565大端
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    // 打开jpeg解码器
    esp_err_t ret = jpeg_dec_open(&config, &jpeg_dec);
    // 如果打开失败，打印错误信息并返回
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "打开解码器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 声明一个jpeg解码器头信息结构体，并初始化为0
    jpeg_dec_header_info_t out_info = {0};
    // 声明一个jpeg解码器IO结构体
    jpeg_dec_io_t io = {
        // 输入缓冲区指针
        .inbuf = jpeg_data, // 传入图像的数据存储
        // 输入缓冲区长度
        .inbuf_len = static_cast<int>(jpeg_size), // 传入图像数据的长度
        // 输入缓冲区剩余长度
        .inbuf_remain = 0,
        // 输出缓冲区指针
        .outbuf = NULL,
        // 输出缓冲区大小
        .out_size = 0};

    // 解析JPEG头获取图像尺寸
    ret = jpeg_dec_parse_header(jpeg_dec, &io, &out_info);
    // 如果解析失败
    if (ret != ESP_OK)
    {
        // 打印错误信息
        ESP_LOGE(TAG, "解析头失败: %s", esp_err_to_name(ret));
        // 关闭JPEG解码器
        jpeg_dec_close(jpeg_dec);
        // 返回
        return ret;
    }
    // 获取输出缓冲区大小
    int outbuf_len = 0;
    ret = jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
    // 如果获取输出缓冲区大小失败
    if (ret != ESP_OK)
    {
        // 打印错误信息
        ESP_LOGE(TAG, "获取输出缓冲区大小失败: %s", esp_err_to_name(ret));
        // 关闭JPEG解码器
        jpeg_dec_close(jpeg_dec);
        // 返回
        return ret;
    }

    // 分配 16 字节对齐的内存（例如用于 DMA 操作）
    // 分配输出缓冲区
    uint16_t *rgb565_buf = (uint16_t *)heap_caps_aligned_alloc(16, out_info.width * out_info.height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // 如果分配失败
    if (!rgb565_buf)
    {
        // 打印错误信息
        ESP_LOGE(TAG, "分配输出缓冲区失败");
        // 关闭JPEG解码器
        jpeg_dec_close(jpeg_dec);
        // 返回
        return ESP_ERR_NO_MEM;
    }

    // 设置输出缓冲区
    io.outbuf = (uint8_t *)rgb565_buf;
    io.out_size = outbuf_len;

    // 执行解码
    ret = jpeg_dec_process(jpeg_dec, &io);

    if (ret == ESP_OK)
    {
        // 更新输出参数
        *rgb565_data = (uint8_t *)rgb565_buf;
        *rgb565_size = outbuf_len;
        *width = out_info.width;
        *height = out_info.height;
    }
    else
    {
        // 解码失败，释放内存
        heap_caps_free(rgb565_buf);
        ESP_LOGE(TAG, "解码失败: %s", esp_err_to_name(ret));
    }
    jpeg_dec_close(jpeg_dec);
    return ret;
}