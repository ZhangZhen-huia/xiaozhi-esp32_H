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

LV_FONT_DECLARE(font_puhui_16_4);
void LcdDisplay::OnlineMusicUI(void){

    DisplayLockGuard lock(this);
    onlinemusic_screen_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(onlinemusic_screen_, 320, 240);
    lv_obj_set_style_radius(onlinemusic_screen_, 0, 0);
    lv_obj_set_style_bg_color(onlinemusic_screen_, lv_color_hex(0x0), 0);
    lv_obj_set_style_border_width(onlinemusic_screen_, 0, 0);
    lv_obj_clear_flag(onlinemusic_screen_, LV_OBJ_FLAG_SCROLLABLE);
    current_screen_ = onlinemusic_screen_;

    lv_obj_t *status_bar = lv_obj_create(onlinemusic_screen_);
    lv_obj_set_size(status_bar, 320, 30);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *label = lv_label_create(status_bar);
    lv_label_set_text(label, "Online Music");
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    
    label_musicname_ = lv_label_create(status_bar);
    lv_label_set_text(label_musicname_, "");
    lv_obj_set_style_text_color(label_musicname_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label_musicname_, &font_puhui_16_4, 0);
    lv_obj_align(label_musicname_, LV_ALIGN_CENTER, 0, 0);

    lyrics_area = lv_obj_create(onlinemusic_screen_);
    lv_obj_set_size(lyrics_area, 300, 180);
    lv_obj_align_to(lyrics_area,status_bar,LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(lyrics_area, 0, 0);
    lv_obj_set_style_bg_color(lyrics_area, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lyrics_area, 0, 0);
    lv_obj_clear_flag(lyrics_area, LV_OBJ_FLAG_SCROLLABLE);

    //把 lyrics_area 的子对象按“主轴”为纵向排列（列方向）
    //也就是说子控件从上到下依次布局
    lv_obj_set_flex_flow(lyrics_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lyrics_area, LV_FLEX_ALIGN_SPACE_EVENLY,
                                        LV_FLEX_ALIGN_CENTER,
                                        LV_FLEX_ALIGN_CENTER);
    
    for (int i = 0; i < 5; i++) {
        lrc_lines[i] = lv_label_create(lyrics_area);
        lv_label_set_text(lrc_lines[i], "");
        lv_label_set_long_mode(lrc_lines[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(lrc_lines[i], LV_TEXT_ALIGN_CENTER, 0);
    }                                    
    lv_obj_add_flag(onlinemusic_screen_, LV_OBJ_FLAG_HIDDEN);
}



void LcdDisplay:: OnlineMusiclrc_refresh(int top_idx,std::vector<std::pair<int, std::string>> lyrics)
{
    DisplayLockGuard lock(this);
    if (top_idx < 0) top_idx = 0;
    if (top_idx < 0) top_idx = 0;

    lrc_top = top_idx;
    for (int i = 0; i < 5; i++) {
        int lyric_idx = top_idx + i;
        const char *txt = (lyric_idx < (int)lyrics.size())
                          ? lyrics[lyric_idx].second.c_str()
                          : "";
        lv_label_set_text(lrc_lines[i], txt);

        //中心加粗
        int dist = abs(i - lrc_cent);
        if(dist == 0)
            {
                static lv_style_t bold_style;
                // 第一次初始化
                if (bold_style.prop_cnt == 0) {          
                    lv_style_init(&bold_style);
                    lv_style_set_text_decor(&bold_style, LV_TEXT_DECOR_NONE);
                    //上下左右各 1 px 描边
                    lv_style_set_text_color(&bold_style, lv_color_hex(0x330000)); //深色边
                    lv_style_set_text_opa(&bold_style, LV_OPA_70);
                }
                lv_obj_add_style(lrc_lines[i], &bold_style, LV_PART_MAIN);
            }
        //中心高亮
        lv_color_t color = (dist == 0) ? lv_color_hex(0x00FF45)
                                        : lv_color_hex(0xAAAAAA);
        lv_obj_set_style_text_color(lrc_lines[i], color, 0);
    }
}


//把容器往下移一行高度 → 再重新填充 → 再动画移回 0
void LcdDisplay:: lrc_animate_next(int new_top)
{
    DisplayLockGuard lock(this);
    if (new_top == lrc_top) return;

    //设置行高
    const int line_h = 23;   

    //瞬间下移一行，视觉上歌词上移一格
    lv_obj_set_y(lyrics_area, line_h);
    auto &board = Board::GetInstance();
    auto music = board.GetMusic();
    //更新内容（此时在屏幕外)
    OnlineMusiclrc_refresh(new_top, music->GetLyrics());

    //动画回到 0 → 产生“整体上移”效果
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lyrics_area);
    lv_anim_set_values(&a, line_h, 0);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}