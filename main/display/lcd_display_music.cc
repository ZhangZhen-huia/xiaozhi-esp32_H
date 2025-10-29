#include "lcd_display.h"
 #include <esp_log.h>
#include <esp_lvgl_port.h>
#include "assets/lang_config.h"
#include <cstring>
#include "settings.h"
#include "application.h"
#include "board.h"


#define TAG "LcdDisplay_music"
LV_IMG_DECLARE(img3);
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
 /* 状态 */
bool is_playing = false;
lv_anim_t anim_cover;

/* -------------- 动画：封面旋转 -------------- */
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
        lv_timer_del(hide_timer);       /* 用完即毁 */
        hide_timer = NULL;
    }, 2000, NULL);
    lv_timer_set_repeat_count(hide_timer, 1);   //只跑一次
}




static bool list_visible = false;  /* 当前状态 */

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
    lv_anim_set_time(&a, 300);                     /* 300 ms */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, [](lv_anim_t * a){
        if(!list_visible) {
        lv_obj_add_flag(list_song, LV_OBJ_FLAG_HIDDEN);  // 动画结束再隐藏
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

void LcdDisplay::MusicUI() 
{
    DisplayLockGuard lock(this);
    
    lv_theme_t *th = lv_theme_default_init(NULL, lv_palette_main(LV_PALETTE_RED),
                                           lv_palette_main(LV_PALETTE_GREY),
                                           true, LV_FONT_DEFAULT);
    lv_disp_set_theme(NULL, th);
    music_screen_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(music_screen_, 320, 240);
    lv_obj_set_style_border_width(music_screen_, 0, 0);
    lv_obj_clear_flag(music_screen_, LV_OBJ_FLAG_SCROLLABLE);
    current_screen_ = music_screen_;
    
    /* 顶部标题 */
    label_title = lv_label_create(music_screen_);
    lv_label_set_text(label_title, "Music LVGL Demo");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 40, 5);

    //图片容器
    lv_obj_t * circle = lv_obj_create(music_screen_);
    lv_obj_set_size(circle, 115, 115);                   /* 直径 115 px */
    lv_obj_align(circle, LV_ALIGN_LEFT_MID, 0, -40);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);/* 变成正圆 */
    lv_obj_set_style_clip_corner(circle, true, 0);       /* 裁剪超出部分 */
    lv_obj_set_style_border_width(circle, 5, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);              /* 去掉内边距 */
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 把图片作为子对象放进来 */
    img_cover = lv_img_create(circle);
    lv_image_set_src(img_cover, &img3);
    lv_obj_center(img_cover);

    hidden_btn = lv_btn_create(music_screen_);
    lv_obj_set_size(hidden_btn, 170,240);
    lv_obj_set_style_bg_opa(hidden_btn,0,LV_PART_MAIN);
    lv_obj_align(hidden_btn,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_add_event_cb(hidden_btn,btn_playlist_cb,LV_EVENT_CLICKED,NULL);

    /* 进度条 */
    slider_progress = lv_slider_create(music_screen_);
    lv_obj_set_size(slider_progress, 250, 5);
    lv_obj_align_to(slider_progress, music_screen_, LV_ALIGN_BOTTOM_LEFT, 5, -60);
    lv_slider_set_range(slider_progress, 0, 100);



    lv_obj_t *volume_btn = lv_btn_create(music_screen_);
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


    slider_vol = lv_slider_create(music_screen_);
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
    lv_obj_t *bar = lv_obj_create(music_screen_);
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
    }, LV_EVENT_CLICKED, NULL);

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
    lv_obj_t *time_label = lv_label_create(music_screen_);
    lv_label_set_text(time_label, "00:00 / 03:45");
    lv_obj_align_to(time_label, slider_progress, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);



    /* 播放列表 */
    list_song = lv_list_create(music_screen_);
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

    lv_obj_add_flag(music_screen_, LV_OBJ_FLAG_HIDDEN);
}