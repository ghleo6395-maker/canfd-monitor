/* LVGL 9.5 bring-up configuration. Build with LV_CONF_INCLUDE_SIMPLE and this
 * directory on the include path. Unspecified options keep LVGL defaults. */
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_MEM_SIZE (96U * 1024U)
#if defined(CANFD_REVD_TARGET) && (defined(__arm__) || defined(__thumb__))
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY __attribute__((section(".revd_d2_bss"), aligned(32)))
#endif
#define LV_DEF_REFR_PERIOD 100
#define LV_DPI_DEF 130
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
/* On allocation/programming faults do not leave the system in a silent spin. */
#define LV_ASSERT_HANDLER_INCLUDE "stdlib.h"
#define LV_ASSERT_HANDLER abort();
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_USE_DRAW_ARM2D 0
#define LV_USE_DRAW_DMA2D 0
#define LV_USE_DRAW_VGLITE 0
#define LV_USE_DRAW_PXP 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_MATRIX 0
#define LV_USE_FLOAT 0
#define LV_USE_ST7796 1
#define LV_USE_GENERIC_MIPI 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(ui_font_revd_16)
#define LV_FONT_DEFAULT &ui_font_revd_16
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0
#define LV_USE_BUTTON 1
#define LV_USE_LABEL 1
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_ANIMATION 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_SYSMON 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#endif
