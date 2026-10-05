/**
 * lv_conf.h — LVGL v8.4 configuration for the Indorep QRIS mini display.
 *
 * WHY THIS FILE IS SO EXPLICIT:
 * LVGL's own lv_conf_template.h enables a large optional surface (PNG, GIF,
 * BMP, SJPG, ffmpeg, freetype, tiny_ttf, rlottie, every font, every widget,
 * the SDL driver, vglite, ...). Every enabled feature becomes another .c file
 * the build has to compile, and lv_conf.h invalidates ALL of them whenever it
 * changes. This file therefore disables everything we do not use, which cuts
 * the LVGL build from ~180 files to a few dozen and makes rebuilds fast.
 *
 * Keep it in sync with what src/ actually calls. If you add a widget, enable
 * exactly that widget here.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOUR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/*====================
   MEMORY SETTINGS
 *====================*/
/* Let LVGL use its own internal allocator (no malloc redirect). */
#define LV_MEM_CUSTOM 0
/* Size budget (this heap lives in INTERNAL RAM; the draw buffers are in PSRAM):
 *   QR canvas (216x216 1-bit indexed)   ~5.8 KB  (allocated for the widget's life)
 *   QR encoder scratch during an update ~1.4 KB  (freed straight after)
 *   ~60 widgets + label text            ~20 KB
 *   LVGL internal bookkeeping/timers    ~6 KB
 * 64 KB leaves comfortable headroom; LV_USE_ASSERT_MALLOC below turns any
 * shortfall into an immediate, logged failure instead of a silent NULL deref. */
#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 16

/*====================
   PERFORMANCE
   Taken from the vendor's own lv_conf.h for this board plus the performance
   notes in the module docs (Performance.pdf, referenced from the Waveshare-style
   notes in 4.3inch_ESP32-4827S043). With LV_MEMCPY_MEMSET_STD and
   LV_ATTRIBUTE_FAST_MEM left at their defaults on a 480x272 RGB panel the UI
   redraw is noticeably slower, so both are set explicitly here.
 *====================*/
/* Use the C library memcpy/memset (faster than LVGL's byte-wise fallback). */
#define LV_MEMCPY_MEMSET_STD 1
/* Place hot drawing functions in IRAM. */
#define LV_ATTRIBUTE_FAST_MEM IRAM_ATTR
/* 64-byte data cache lines on the ESP32-S3. */
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY

/*====================
   HAL SETTINGS
 *====================*/
/* Drive lv_tick from Arduino millis(). */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

/*====================
   LOGGING
 *====================*/
#define LV_USE_LOG 0

/*====================
   ASSERTS
 *====================*/
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/*====================
   FONTS
   Only the sizes the UI uses. LV_FONT_MONTSERRAT_14 is the default face.
 *====================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK 0
#define LV_FONT_UNSCII_8 0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0
#define LV_USE_FONT_PLACEHOLDER 1

/*====================
   TEXT SETTINGS
 *====================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*====================
   RUN TIME SETTINGS
 *====================*/
/* Refresh period in ms. 30 ms = ~33 fps.
 *
 * This is the ceiling on how often LVGL redraws. It is deliberately NOT smaller:
 * every redraw becomes an RGB panel flush, and the panel is driven through the
 * same PSRAM the Wi-Fi and CPU work contend for. 33 fps is smooth for this UI
 * (static screens with occasional taps) and keeps the flush rate low. */
#define LV_DISP_DEF_REFR_PERIOD 30

/*====================
   WIDGETS
   Only what the UI uses. Each disabled widget removes a .c file.
 *====================*/
#define LV_USE_ARC        0   /* NOT used (was only the spinner's base class) */
#define LV_USE_BAR        0   /* NOT used */
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  0
#define LV_USE_CANVAS     1   /* QR code is drawn into a canvas */
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMG        1   /* QR shown as an image */
#define LV_USE_LABEL      1
/* Line: a straight segment between two points.
 *
 * Needed for two things, both of which are otherwise impossible with the fonts
 * available:
 *   1. the diagonal slash that turns LV_SYMBOL_WIFI into the "disconnected"
 *      icon. Montserrat has no wifi-off glyph.
 *   2. the analog clock hands on the homescreen.
 *
 * Costs one small .c file. */
#define LV_USE_LINE       1
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TEXTAREA   0
#define LV_USE_TABLE      0

/*====================
   EXTRA (optional) WIDGETS — all off
 *====================*/
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0   /* NOT used: an animated spinner is a permanent
                               * redraw for information three static dots convey
                               * just as well. The connecting screen uses a
                               * plain label instead (see ui_screens.cpp). */
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

/*====================
   THEMES
 *====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0

/*====================
   LAYOUTS
 *====================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================
   FILE SYSTEM / DECODERS — all off
   The QR is generated in RAM, nothing is loaded from disk.
 *====================*/
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_PNG 0
#define LV_USE_BMP 0
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 1   /* built-in QR generator, used for the QRIS payload */
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_FFMPEG 0

/*====================
   OTHERS
 *====================*/
#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_MSG 0
#define LV_USE_IME_PINYIN 0
#define LV_USE_OBJ_PROPERTY 0

/*====================
   GPU / ACCELERATION — none on ESP32-S3
 *====================*/
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_SWM341_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0
#define LV_USE_GPU_ARM2D 0

/*====================
   DEBUG / PERF
 *====================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

#endif /* LV_CONF_H */
