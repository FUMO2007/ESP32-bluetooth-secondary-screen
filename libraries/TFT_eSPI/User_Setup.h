#define USER_SETUP_ID 99

// 1. 只保留你的屏幕驱动
#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// 2. 修正引脚定义（根据你的代码：MOSI=21, SCLK=16, CS=19, DC=14, RST=10, BL=12）
#define TFT_MOSI  21
#define TFT_SCLK  16
#define TFT_CS    19    // 确保数字后面没有空格
#define TFT_DC    14
#define TFT_RST   10
#define TFT_BL    12

#define TFT_BACKLIGHT_ON HIGH

// 3. 选择一种SPI模式（推荐硬件SPI）
#define USE_HSPI_PORT      // 使用硬件SPI，更稳定
// #define USE_VSPI_PORT   // 如果要用VSPI，取消注释这行，注释掉上一行

// 4. 硬件SPI可以跑更高频率
#define USE_HSPI_PORT
#define SPI_FREQUENCY  60000000
#define SPI_READ_FREQUENCY 20000000
// 开启DMA硬件传输，大幅加快整屏推送速度
#define ESP32_DMA_SUPPORT
#define TFT_RGB_ORDER TFT_BGR
// 5. 保留必要字体
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
// #define LOAD_FONT6      // 如果内存紧张，可以注释掉不用的
// #define LOAD_FONT7
// #define LOAD_FONT8
// #define SMOOTH_FONT     // 平滑字体很占内存，建议先关闭

// 6. 删除其他驱动定义
// #define ILI9341_DRIVER
// #define ST7735_DRIVER
// #define GC9A01_DRIVER
// #define TFT_PARALLEL_8_BIT