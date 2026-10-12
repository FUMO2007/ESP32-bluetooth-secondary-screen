// ============================================================================
//  tft_setup_check.cpp —— 屏幕引脚表的编译期自检
// ----------------------------------------------------------------------------
//  为什么需要它：
//    tft_setup.h 是**本项目唯一的引脚定义处**，但它只在真实 Arduino/ESP32
//    编译里被 TFT_eSPI.h 的 __has_include(<tft_setup.h>) 包含。本仓库 build.ps1
//    的固件语法检查走的是 tools/ui_preview/ 下的桩头（宿主机 g++），
//    于是这个文件以前从来没有被编译过：多打一个数字、把某一行注释掉、
//    换成备选接线时只放开了一半 —— 不会有任何东西报警，只能烧上去看花屏。
//
//  本文件检查的是**不变量**，故意不把 21/16/19 这些具体数字再抄一遍：
//  换接线时只改 tft_setup.h，这里不用动，也不会因为"你换了正常接线"而失败。
//  由 build.ps1 第 4 步用 -fsyntax-only -Wall -Wextra 编译（零告警约定）。
// ============================================================================
#include "tft_setup.h"

// ---------------------------------------------------------------- 1. 必需宏
// 漏定义必须在这里就炸，而不是烧到板子上才发现屏幕点不亮。
#ifndef TFT_WIDTH
#error "tft_setup.h 缺少 TFT_WIDTH"
#endif
#ifndef TFT_HEIGHT
#error "tft_setup.h 缺少 TFT_HEIGHT"
#endif
#ifndef TFT_MOSI
#error "tft_setup.h 缺少 TFT_MOSI"
#endif
#ifndef TFT_SCLK
#error "tft_setup.h 缺少 TFT_SCLK"
#endif
#ifndef TFT_CS
#error "tft_setup.h 缺少 TFT_CS"
#endif
#ifndef TFT_DC
#error "tft_setup.h 缺少 TFT_DC"
#endif
#ifndef TFT_RST
#error "tft_setup.h 缺少 TFT_RST"
#endif
#ifndef TFT_BL
#error "tft_setup.h 缺少 TFT_BL（screen_draw.cpp 用它初始化背光）"
#endif

// 驱动宏必须且只能有一个（写成三元是为了不依赖 defined() 的宏展开行为）
#if ((defined(ST7789_DRIVER) ? 1 : 0) + (defined(ILI9341_DRIVER) ? 1 : 0) +     \
     (defined(ST7735_DRIVER) ? 1 : 0) + (defined(ILI9488_DRIVER) ? 1 : 0) +     \
     (defined(ILI9486_DRIVER) ? 1 : 0) + (defined(GC9A01_DRIVER) ? 1 : 0)) != 1
#error "必须且只能定义一个驱动宏（ST7789_DRIVER / ILI9341_DRIVER / ...）"
#endif

// ---------------------------------------------------------------- 2. 引脚合法性
// ESP32-S3 的可用 GPIO 是 0..48；负值只有 TFT_MISO 允许（-1 = 不接线）。
#define CHECK_PIN_RANGE(p)                                                     \
  static_assert((p) >= 0 && (p) <= 48,                                         \
                #p " 超出 ESP32-S3 的 GPIO 范围 (0..48)")
#define CHECK_PIN_DISTINCT(a, b)                                               \
  static_assert((a) != (b), #a " 与 " #b " 占用了同一个 GPIO")

CHECK_PIN_RANGE(TFT_MOSI);
CHECK_PIN_RANGE(TFT_SCLK);
CHECK_PIN_RANGE(TFT_CS);
CHECK_PIN_RANGE(TFT_DC);
CHECK_PIN_RANGE(TFT_RST);
CHECK_PIN_RANGE(TFT_BL);

CHECK_PIN_DISTINCT(TFT_MOSI, TFT_SCLK);
CHECK_PIN_DISTINCT(TFT_MOSI, TFT_CS);
CHECK_PIN_DISTINCT(TFT_MOSI, TFT_DC);
CHECK_PIN_DISTINCT(TFT_MOSI, TFT_RST);
CHECK_PIN_DISTINCT(TFT_MOSI, TFT_BL);
CHECK_PIN_DISTINCT(TFT_SCLK, TFT_CS);
CHECK_PIN_DISTINCT(TFT_SCLK, TFT_DC);
CHECK_PIN_DISTINCT(TFT_SCLK, TFT_RST);
CHECK_PIN_DISTINCT(TFT_SCLK, TFT_BL);
CHECK_PIN_DISTINCT(TFT_CS, TFT_DC);
CHECK_PIN_DISTINCT(TFT_CS, TFT_RST);
CHECK_PIN_DISTINCT(TFT_CS, TFT_BL);
CHECK_PIN_DISTINCT(TFT_DC, TFT_RST);
CHECK_PIN_DISTINCT(TFT_DC, TFT_BL);
CHECK_PIN_DISTINCT(TFT_RST, TFT_BL);

#ifdef TFT_MISO
// SDO 未接时写 -1（不占引脚）；接了就必须是合法且不冲突的 GPIO。
static_assert(TFT_MISO == -1 || (TFT_MISO >= 0 && TFT_MISO <= 48),
              "TFT_MISO 只能是 -1（不接线）或 0..48 的合法 GPIO");
static_assert(TFT_MISO == -1 || TFT_MISO != TFT_MOSI,
              "TFT_MISO 与 TFT_MOSI 不能是同一个 GPIO");
#endif

// ---------------------------------------------------------------- 3. 分辨率
// 面板按竖屏原始分辨率 240x320 定义，靠 setRotation(1) 出横屏 320x240；
// screen_draw.cpp 的全部布局常量（卡片 x4..157 / x162..315、趋势图 x26..311 …）
// 都是按 320x240 硬算的，改分辨率必须连 UI 一起改。
static_assert(TFT_WIDTH == 240 && TFT_HEIGHT == 320,
              "面板分辨率按 240x320 竖屏定义（setRotation(1) 出横屏 320x240）；"
              "改这里必须同步改 screen_draw.cpp 的布局常量与仿真断言");
