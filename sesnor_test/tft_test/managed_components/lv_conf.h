/**
 * lv_conf.h — Minimal LVGL 8.3 config for ESP32-S3 TFT test
 * Activated by LV_CONF_INCLUDE_SIMPLE (set in top-level CMakeLists.txt)
 */
#if 1  /* keep as 1 — required by LVGL */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ── Color ──────────────────────────────────────────────────────────────── */
#define LV_COLOR_DEPTH     16   /* RGB565 — matches the TFT */
#define LV_COLOR_16_SWAP    1   /* swap bytes so ST7789 (big-endian) gets correct RGB565 */
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/* ── Memory ─────────────────────────────────────────────────────────────── */
#define LV_MEM_CUSTOM  0
#define LV_MEM_SIZE   (64U * 1024U)   /* 64 KB internal heap for LVGL */
#define LV_MEM_ADR     0
#define LV_MEM_POOL_INCLUDE <stdlib.h>
#define LV_MEM_POOL_ALLOC   malloc
#define LV_MEM_POOL_FREE    free
#define LV_MEM_BUF_MAX_NUM  16
#define LV_MEMCPY_MEMSET_STD 0

/* ── HAL ────────────────────────────────────────────────────────────────── */
#define LV_DISP_DEF_REFR_PERIOD  30   /* ms */
#define LV_INDEV_DEF_READ_PERIOD 30   /* ms */
#define LV_TICK_CUSTOM 0
#define LV_DPI_DEF     130

/* ── Draw ───────────────────────────────────────────────────────────────── */
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE  0
#define LV_CIRCLE_CACHE_SIZE  4
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DITHER_GRADIENT 0
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/* ── GPU (disabled) ─────────────────────────────────────────────────────── */
#define LV_USE_GPU_STM32_DMA2D  0
#define LV_USE_GPU_SWM341_DMA2D 0
#define LV_USE_GPU_NXP_PXP      0
#define LV_USE_GPU_NXP_VG_LITE  0
#define LV_USE_GPU_SDL          0

/* ── Logging / asserts ──────────────────────────────────────────────────── */
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE <assert.h>
#define LV_ASSERT_HANDLER abort();

/* ── Fonts ──────────────────────────────────────────────────────────────── */
#define LV_FONT_MONTSERRAT_8   0
#define LV_FONT_MONTSERRAT_10  0
#define LV_FONT_MONTSERRAT_12  0
#define LV_FONT_MONTSERRAT_14  1   /* default */
#define LV_FONT_MONTSERRAT_16  0
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  0
#define LV_FONT_MONTSERRAT_22  0
#define LV_FONT_MONTSERRAT_24  1   /* used for the "xio is great" label */
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  0
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  0
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  0
#define LV_FONT_MONTSERRAT_12_SUBPX       0
#define LV_FONT_MONTSERRAT_28_COMPRESSED  0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW  0
#define LV_FONT_SIMSUN_16_CJK             0
#define LV_FONT_UNSCII_8                  0
#define LV_FONT_UNSCII_16                 0
#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT          &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE    0
#define LV_USE_FONT_SUBPX        0
#define LV_FONT_SUBPX_BGR        0
#define LV_USE_USER_DATA         1

/* ── Text ───────────────────────────────────────────────────────────────── */
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS                " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN        0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI                       0
#define LV_USE_ARABIC_PERSIAN_CHARS       0

/* ── Widgets ────────────────────────────────────────────────────────────── */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1

/* ── Themes ─────────────────────────────────────────────────────────────── */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK             0
#define LV_THEME_DEFAULT_GROW             1
#define LV_THEME_DEFAULT_TRANSITION_TIME  80
#define LV_USE_THEME_SIMPLE 1
#define LV_USE_THEME_MONO   1

/* ── Layouts ────────────────────────────────────────────────────────────── */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* ── Extra widgets ──────────────────────────────────────────────────────── */
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_SNAPSHOT   0
#define LV_USE_MONKEY     0
#define LV_USE_GRIDNAV    0
#define LV_USE_FRAGMENT   0
#define LV_USE_IMGFONT    0
#define LV_USE_MSG        0
#define LV_USE_IME_PINYIN 0

/* ── Debug / perf ───────────────────────────────────────────────────────── */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_REFR_DEBUG   0
#define LV_BUILD_EXAMPLES   0

#endif /* LV_CONF_H */
#endif /* set to 1 */
