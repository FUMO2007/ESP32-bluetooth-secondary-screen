// 仅用于离线语法/类型检查 + 内存预览的 TFT_eSPI 桩
// 签名照抄 libraries/TFT_eSPI/TFT_eSPI.h 与 Extensions/Sprite.h
#pragma once
#include <Arduino.h>

// 背光等引脚定义只有一处：直接包含真实的 PC_Monitor/tft_setup.h
// （编译时 -I PC_Monitor 已提供该路径）。以前这里写死 #define TFT_BL 12 ——
// 改引脚时桩会和真配置悄悄不一致：仿真编得过，板子烧上去却是错的。
#include "tft_setup.h"

// 文本基准（真实库里是 #define）
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8
#define L_BASELINE 9
#define C_BASELINE 10
#define R_BASELINE 11

#define CP437_SWITCH 1
#define UTF8_SWITCH 2
#define PSRAM_ENABLE 3

class TFT_eSPI {
public:
  TFT_eSPI() {}

  // --- 仅预览用：像素缓冲与文本状态（真实库里是私有成员）---
  uint16_t *px = nullptr;
  int bw = 0, bh = 0;
  uint8_t textfont = 1, textsize = 1, textdatum = 0;
  uint16_t textcolor = 0xFFFF, textbgcolor = 0xFFFF;
  void _bind(uint16_t *buf, int w, int h) { px = buf; bw = w; bh = h; }

  void init(void);
  void setRotation(uint8_t r);
  void setAttribute(uint8_t id = 0, uint8_t a = 0);
  uint8_t getAttribute(uint8_t id = 0);
  int16_t width(void) const { return (int16_t)bw; }
  int16_t height(void) const { return (int16_t)bh; }

  void fillScreen(uint32_t color);
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
  void fillRectVGradient(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color1, uint32_t color2);
  void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color);
  void drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color);
  void drawPixel(int32_t x, int32_t y, uint32_t color);
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color);
  void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color);

  void setTextColor(uint16_t color);
  void setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill = false);
  void setTextSize(uint8_t size);
  void setTextDatum(uint8_t datum);
  void setTextFont(uint8_t font);
  void setTextPadding(uint16_t x_width);

  int16_t fontHeight(uint8_t font);
  int16_t textWidth(const char *string, uint8_t font);
  int16_t textWidth(const char *string);
  int16_t textWidth(const String &string, uint8_t font);
  int16_t textWidth(const String &string);

  int16_t drawString(const char *string, int32_t x, int32_t y, uint8_t font);
  int16_t drawString(const char *string, int32_t x, int32_t y);
  int16_t drawString(const String &string, int32_t x, int32_t y, uint8_t font);
  int16_t drawString(const String &string, int32_t x, int32_t y);
};

class TFT_eSprite : public TFT_eSPI {
public:
  TFT_eSprite(TFT_eSPI *tft) { host = tft; }
  TFT_eSPI *host = nullptr;
  uint16_t *sprBuf = nullptr;

  void *createSprite(int16_t width, int16_t height, uint8_t frames = 1);
  bool created(void) { return sprBuf != nullptr; }
  void setColorDepth(int8_t b) { (void)b; }
  void fillSprite(uint32_t color) { fillRect(0, 0, bw, bh, color); }
  void pushSprite(int32_t x, int32_t y);
  void pushSprite(int32_t x, int32_t y, uint16_t transparent);
  bool pushSprite(int32_t tx, int32_t ty, int32_t sx, int32_t sy, int32_t sw, int32_t sh);
};
