// ============================================================================
//  tft_setup.h —— 屏幕硬件配置（本项目唯一的引脚定义处）
// ----------------------------------------------------------------------------
//  为什么是这个名字、这个位置：
//    TFT_eSPI.h 里有 __has_include(<tft_setup.h>)，只要 sketch 目录下存在这个
//    文件，它就会被优先包含，并置上 USER_SETUP_LOADED 标志，于是
//    libraries/TFT_eSPI/User_Setup.h 会被 User_Setup_Select.h 整个跳过。
//    所以：改屏幕接线只改这一个文件，不要再去改库里的 User_Setup.h，
//    也不要在 config.h 里重复定义 TFT_* 宏。
//
//  当前生效的接线（ESP32-S3）：
//    MOSI=21  SCLK=16  CS=19  DC=14  RST=10  BL=12  SDO 未接
// ============================================================================

#define USER_SETUP_ID 99

// ---------- 1. 驱动与面板分辨率（按竖屏原始分辨率填写）----------
#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320      // 与 setRotation(1) 配合 => 横屏 320x240

// ---------- 2. 引脚定义（唯一一处）----------
#define TFT_MOSI  21        // SDA
#define TFT_SCLK  16
#define TFT_CS    19
#define TFT_DC    14
#define TFT_RST   10
#define TFT_BL    12        // 背光
#define TFT_MISO  -1        // 屏幕 SDO 未接，不做读操作（-1 表示不占用引脚）

// 备选接线：以下是另一版原理图（EasyEDA 工程）里的接法。
// 换板子时把上面 6 行注释掉、把下面 6 行放开即可，其它文件都不用动。
// #define TFT_MOSI  37
// #define TFT_SCLK  35
// #define TFT_CS    10
// #define TFT_DC    11
// #define TFT_RST   12
// #define TFT_BL    13

#define TFT_BACKLIGHT_ON HIGH   // TFT_eSPI::init() 会据此把 TFT_BL 拉高

// ---------- 3. SPI 总线 ----------
#define USE_HSPI_PORT           // ESP32-S3 上对应 SPI3_HOST
#define SPI_FREQUENCY       60000000
#define SPI_READ_FREQUENCY  20000000
// 说明：ESP32-S3 的 SPI 模式下 DMA 由库自动开启
//       （Processors/TFT_eSPI_ESP32_S3.h 中直接 #define ESP32_DMA），
//       并不存在 ESP32_DMA_SUPPORT 这个宏，写了也不生效，故不再保留。

// ---------- 4. 颜色顺序 ----------
#define TFT_RGB_ORDER TFT_BGR   // 若显示为红蓝反色，改成 TFT_RGB

// ---------- 5. 字体（按需裁剪，省 flash/内存）----------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
// #define LOAD_FONT6
// #define LOAD_FONT7
// #define LOAD_FONT8
// #define SMOOTH_FONT
