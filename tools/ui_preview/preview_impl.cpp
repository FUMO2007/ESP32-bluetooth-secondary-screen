// ============================================================================
//  离线预览实现：用内存帧缓冲实现 TFT_eSPI 桩，让真实的 screen_draw.cpp
//  在没有硬件的情况下把界面画出来（字体 1 用真实点阵，字体 2/4 用等宽占位块，
//  只为验证布局几何：位置、是否压字、是否越界）。
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "TFT_eSPI.h"
#include "fonts_gen.h"

uint16_t SCREEN[320 * 240];

// ---- 绘制计数：用来验证"频闪修复"是否真的生效 ----
// textOnScreen > 0 说明还有文字是直接画到屏幕上的（先擦后画 -> 会有空窗微闪）
// fillScreenCalls 只应在 screenInit 里出现 1 次（切页不清屏）
static int g_textOnScreen = 0;
static int g_fillScreenCalls = 0;
int  simTextOnScreen(void) { return g_textOnScreen; }
int  simFillScreenCalls(void) { return g_fillScreenCalls; }
void simResetCounters(void) { g_textOnScreen = 0; }

static unsigned long g_millis = 0;
_SerialStub Serial;
_EspStub ESP;
unsigned long millis(void) { return g_millis; }
void simAdvanceMs(unsigned long ms) { g_millis += ms; }
void delay(unsigned long) {}
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t, uint8_t) {}

// ---------------- 文本度量 ----------------
static int glyphAdvance(uint8_t font, unsigned char c) {
  if (font == 1) return 6;
  unsigned char u = c;
  if (u < 32 || u > 127) u = 32;
  if (font == 2) return WIDTBL_F16[u - 32];
  if (font == 4) return WIDTBL_F32[u - 32];
  return 6;
}

static int glyphHeight(uint8_t font) {
  if (font == 1) return 8;
  if (font == 2) return 16;
  return 26;                                  // font 4
}

// ---------------- 基础图元 ----------------
static inline void blit(TFT_eSPI &d, int x, int y, uint16_t c) {
  if (!d.px || x < 0 || y < 0 || x >= d.bw || y >= d.bh) return;
  d.px[y * d.bw + x] = c;
}

void TFT_eSPI::init(void) {}
void TFT_eSPI::setRotation(uint8_t) {}
void TFT_eSPI::setAttribute(uint8_t, uint8_t) {}
uint8_t TFT_eSPI::getAttribute(uint8_t) { return 0; }
void TFT_eSPI::setTextColor(uint16_t c) { textcolor = c; textbgcolor = c; }
void TFT_eSPI::setTextColor(uint16_t fg, uint16_t bg, bool) { textcolor = fg; textbgcolor = bg; }
void TFT_eSPI::setTextSize(uint8_t s) { textsize = s ? s : 1; }
void TFT_eSPI::setTextDatum(uint8_t d) { textdatum = d; }
void TFT_eSPI::setTextFont(uint8_t f) { textfont = f; }
void TFT_eSPI::setTextPadding(uint16_t) {}
int16_t TFT_eSPI::fontHeight(uint8_t f) { return glyphHeight(f) * textsize; }

void TFT_eSPI::drawPixel(int32_t x, int32_t y, uint32_t c) { blit(*this, (int)x, (int)y, (uint16_t)c); }

void TFT_eSPI::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) blit(*this, (int)x + i, (int)y + j, (uint16_t)c);
}

void TFT_eSPI::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
  fillRect(x, y, w, 1, c); fillRect(x, y + h - 1, w, 1, c);
  fillRect(x, y, 1, h, c); fillRect(x + w - 1, y, 1, h, c);
}

void TFT_eSPI::fillScreen(uint32_t c) {
  if (px == SCREEN) g_fillScreenCalls++;
  fillRect(0, 0, bw, bh, c);
}

void TFT_eSPI::fillRectVGradient(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c1, uint32_t c2) {
  for (int j = 0; j < h; j++) {
    double a = (h <= 1) ? 0.0 : (double)j / (double)(h - 1);
    int r1 = ((c1 >> 11) & 0x1F) * 255 / 31, g1 = ((c1 >> 5) & 0x3F) * 255 / 63, b1 = (c1 & 0x1F) * 255 / 31;
    int r2 = ((c2 >> 11) & 0x1F) * 255 / 31, g2 = ((c2 >> 5) & 0x3F) * 255 / 63, b2 = (c2 & 0x1F) * 255 / 31;
    int r = (int)(r1 + (r2 - r1) * a), g = (int)(g1 + (g2 - g1) * a), b = (int)(b1 + (b2 - b1) * a);
    uint16_t cc = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    fillRect(x, y + j, w, 1, cc);
  }
}

void TFT_eSPI::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t c) { fillRect(x, y, w, 1, c); }
void TFT_eSPI::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t c) { fillRect(x, y, 1, h, c); }

void TFT_eSPI::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
  int dx = abs((int)x1 - (int)x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs((int)y1 - (int)y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    blit(*this, (int)x0, (int)y0, (uint16_t)c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void TFT_eSPI::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t c) {
  for (int j = -r; j <= r; j++)
    for (int i = -r; i <= r; i++)
      if (i * i + j * j <= r * r) blit(*this, (int)x + i, (int)y + j, (uint16_t)c);
}

void TFT_eSPI::drawCircle(int32_t x, int32_t y, int32_t r, uint32_t c) {
  for (int a = 0; a < 360; a += 8)
    blit(*this, (int)(x + r * cos(a * 3.14159265 / 180.0)), (int)(y + r * sin(a * 3.14159265 / 180.0)), (uint16_t)c);
}

// 真实 TFT_eSPI 的圆角是用四分之一圆拼的，像素不会越出矩形——桩也必须这样，
// 否则会在矩形外留下假像素，误导布局判断。
void TFT_eSPI::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int cx = (i < r) ? r : (i >= w - r ? w - r - 1 : i);
      int cy = (j < r) ? r : (j >= h - r ? h - r - 1 : j);
      int dx = i - cx, dy = j - cy;
      if (dx * dx + dy * dy <= r * r) blit(*this, (int)x + i, (int)y + j, (uint16_t)c);
    }
  }
}

void TFT_eSPI::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  drawFastHLine(x + r, y, w - 2 * r, c);
  drawFastHLine(x + r, y + h - 1, w - 2 * r, c);
  drawFastVLine(x, y + r, h - 2 * r, c);
  drawFastVLine(x + w - 1, y + r, h - 2 * r, c);
  for (int j = 0; j < r; j++) {
    for (int i = 0; i < r; i++) {
      int dx = r - i, dy = r - j;
      if (dx * dx + dy * dy <= r * r && dx * dx + dy * dy > (r - 1) * (r - 1)) {
        blit(*this, (int)x + i, (int)y + j, (uint16_t)c);                 // 左上
        blit(*this, (int)x + w - 1 - i, (int)y + j, (uint16_t)c);         // 右上
        blit(*this, (int)x + i, (int)y + h - 1 - j, (uint16_t)c);         // 左下
        blit(*this, (int)x + w - 1 - i, (int)y + h - 1 - j, (uint16_t)c); // 右下
      }
    }
  }
}

// ---------------- 文本 ----------------
int16_t TFT_eSPI::textWidth(const char *s, uint8_t font) {
  int w = 0;
  while (*s) w += glyphAdvance(font, (unsigned char)*s++);
  return (int16_t)(w * textsize);
}
int16_t TFT_eSPI::textWidth(const char *s) { return textWidth(s, textfont); }
int16_t TFT_eSPI::textWidth(const String &s, uint8_t font) { return textWidth(s.c_str(), font); }
int16_t TFT_eSPI::textWidth(const String &s) { return textWidth(s.c_str(), textfont); }

static void drawGlyph(TFT_eSPI &d, int x, int y, unsigned char ch, uint8_t font, uint8_t size, uint16_t col) {
  if (font == 1) {
    // 真实 GLCD 点阵：5 列 x 8 行，bit0 是最上面一行
    const unsigned char *gl = &GLCD_FONT[(unsigned)(ch % 256) * 5];
    for (int kx = 0; kx < 5; kx++)
      for (int ry = 0; ry < 8; ry++)
        if (gl[kx] & (1 << ry))
          for (int sy = 0; sy < size; sy++)
            for (int sx = 0; sx < size; sx++)
              blit(d, x + kx * size + sx, y + ry * size + sy, col);
  } else {
    // 字体 2/4：只画等宽占位块（棋盘格），用于验证几何
    int w = glyphAdvance(font, ch) * size;
    int h = glyphHeight(font) * size;
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++)
        if (((i / 2) + (j / 2)) % 2 == 0) blit(d, x + i, y + j, col);
  }
}

int16_t TFT_eSPI::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (px == SCREEN) g_textOnScreen++;          // 直接画到屏幕上的文字（局部刷新不该出现）
  int16_t cw = textWidth(s, font);
  int16_t chh = (int16_t)(glyphHeight(font) * textsize);
  if (textdatum == TR_DATUM || textdatum == MR_DATUM || textdatum == BR_DATUM) x -= cw;
  else if (textdatum == TC_DATUM || textdatum == MC_DATUM || textdatum == BC_DATUM) x -= cw / 2;
  if (textdatum == BL_DATUM || textdatum == BC_DATUM || textdatum == BR_DATUM) y -= chh;
  else if (textdatum == ML_DATUM || textdatum == MC_DATUM || textdatum == MR_DATUM) y -= chh / 2;

  int cx = (int)x;
  while (*s) {
    unsigned char ch = (unsigned char)*s++;
    if (ch >= 32) drawGlyph(*this, cx, (int)y, ch, font, textsize, textcolor);
    cx += glyphAdvance(font, ch) * textsize;
  }
  return cw;
}
int16_t TFT_eSPI::drawString(const char *s, int32_t x, int32_t y) { return drawString(s, x, y, textfont); }
int16_t TFT_eSPI::drawString(const String &s, int32_t x, int32_t y, uint8_t f) { return drawString(s.c_str(), x, y, f); }
int16_t TFT_eSPI::drawString(const String &s, int32_t x, int32_t y) { return drawString(s.c_str(), x, y, textfont); }

// ---------------- Sprite ----------------
void *TFT_eSprite::createSprite(int16_t w, int16_t h, uint8_t) {
  sprBuf = (uint16_t *)calloc((size_t)w * h, 2);
  _bind(sprBuf, w, h);
  return sprBuf;
}
void TFT_eSprite::pushSprite(int32_t x, int32_t y) {
  if (!sprBuf || !host) return;
  for (int j = 0; j < bh; j++)
    for (int i = 0; i < bw; i++) {
      int tx = (int)x + i, ty = (int)y + j;
      if (tx < 0 || ty < 0 || tx >= host->bw || ty >= host->bh) continue;
      host->px[ty * host->bw + tx] = sprBuf[j * bw + i];
    }
}

// 子矩形推送：语义与真实库一致 pushSprite(目标x, 目标y, 源x, 源y, 源宽, 源高)
bool TFT_eSprite::pushSprite(int32_t tx, int32_t ty, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
  if (!sprBuf || !host) return false;
  for (int j = 0; j < sh; j++)
    for (int i = 0; i < sw; i++) {
      int sxi = (int)sx + i, syj = (int)sy + j;
      if (sxi < 0 || syj < 0 || sxi >= bw || syj >= bh) continue;
      int x = (int)tx + i, y = (int)ty + j;
      if (x < 0 || y < 0 || x >= host->bw || y >= host->bh) continue;
      host->px[y * host->bw + x] = sprBuf[syj * bw + sxi];
    }
  return true;
}

void TFT_eSprite::pushSprite(int32_t x, int32_t y, uint16_t) { pushSprite(x, y); }

// ---------------- 颜色字符化 / 断言 ----------------
static char colorChar(uint16_t c) {
  switch (c) {
    case 0x0000: return ' ';   // C_BG
    case 0x18E3: return '.';   // C_CARD（也是标题栏渐变起点）
    case 0x2965: return ':';   // C_CARD_ED
    case 0x10A2: return '=';   // C_PILL_BG
    case 0x0861: return ',';   // C_PLOT_BG
    case 0x0841: return ';';   // C_BAR_BG
    case 0x2945: return '+';   // C_GRID
    case 0xFFFF: return 'W';   // C_VALUE
    case 0xBDF7: return 'l';   // C_LABEL
    case 0xD69A: return 'd';   // C_LABEL_DIM（次要文字/刻度）
    case 0x8410: return 'x';   // C_VALUE_DIM
    case 0x7FE0: return 'C';   // C_CPU
    case 0xFB00: return 'G';   // C_GPU
    case 0x07E0: return 'O';   // C_OK
    case 0xFFE0: return 'Y';   // C_WARN
    case 0xF800: return 'R';   // C_ALERT
    case 0xFD20: return 'o';   // C_ORANGE
    case 0x07FF: return 'b';   // C_CYAN
    case 0x0460: return 'c';   // C_CPU_DIM
    case 0x8800: return 'g';   // C_GPU_DIM
    case 0x1CAF: return '#';   // 标题栏底线
    default:     return '?';
  }
}

// 单元格取"最显眼"的颜色：前景色优先于背景色
static int colorRank(char ch) {
  switch (ch) {
    case ' ': return 0;
    case '.': return 1;
    case ',': return 2;
    case ';': return 2;
    case '-': return 3;
    case '=': return 3;
    case ':': return 4;
    case '#': return 4;
    case '+': return 5;
    case 'c': case 'g': return 6;
    case 'd': return 7;
    case 'l': return 7;
    case 'x': return 7;
    case 'o': return 8;
    case 'b': return 8;
    case 'O': return 9;
    case 'Y': return 10;
    case 'W': return 11;
    case 'C': case 'G': return 12;
    case 'R': return 13;
    default:  return 1;
  }
}

void simDumpRegion(const char *title, int x0, int y0, int x1, int y1, int scale) {
  printf("\n=== %s  (x %d..%d, y %d..%d, 每格 %dx%d) ===\n", title, x0, x1 - 1, y0, y1 - 1, scale, scale);
  for (int y = y0; y < y1; y += scale) {
    printf("%3d|", y);
    for (int x = x0; x < x1; x += scale) {
      int cnt[256] = {0};
      for (int j = 0; j < scale && y + j < y1; j++)
        for (int i = 0; i < scale && x + i < x1; i++)
          cnt[(unsigned char)colorChar(SCREEN[(y + j) * 320 + (x + i)])]++;
      char best = ' '; int bestRank = -1;
      for (int k = 0; k < 256; k++) {
        if (!cnt[k]) continue;
        int r = colorRank((char)k);
        if (r > bestRank) { bestRank = r; best = (char)k; }
      }
      putchar(best);
    }
    putchar('\n');
  }
}

// 断言：某个矩形内是否出现指定颜色
int simCountColor(int x0, int y0, int x1, int y1, uint16_t color) {
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
      if (SCREEN[y * 320 + x] == color) n++;
  return n;
}

int simCountColors(int x0, int y0, int x1, int y1, const uint16_t *cols, int ncol) {
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
      for (int c = 0; c < ncol; c++)
        if (SCREEN[y * 320 + x] == cols[c]) { n++; break; }
  return n;
}

// 某矩形内"最靠右"的非背景像素的 x（-1 表示没有）
int simRightmostNonBg(int x0, int y0, int x1, int y1, uint16_t bg) {
  for (int x = x1 - 1; x >= x0; x--)
    for (int y = y0; y < y1; y++)
      if (SCREEN[y * 320 + x] != bg) return x;
  return -1;
}

// 某颜色的包围盒（验证元素实际落点）
void simBBox(const char *name, uint16_t color) {
  int x0 = 9999, y0 = 9999, x1 = -1, y1 = -1, n = 0;
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 320; x++)
      if (SCREEN[y * 320 + x] == color) {
        n++;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  if (!n) { printf("  %-16s 未出现\n", name); return; }
  printf("  %-16s 像素=%5d  x %3d..%3d  y %3d..%3d\n", name, n, x0, x1, y0, y1);
}

// 把候选字符串用同一套渲染管线画进空白缓冲，再按同样的窗口打印墨迹图（用于和实际屏幕对照）
void simInkMapExpected(const char *text, int tx, int ty, int x0, int y0, int x1, int y1) {
  static TFT_eSPI lb;
  static uint16_t *buf = nullptr;
  if (!buf) {
    buf = (uint16_t *)calloc(320 * 240, 2);
    lb._bind(buf, 320, 240);
  }
  memset(buf, 0, 320 * 240 * 2);
  lb.setTextFont(1);
  lb.setTextSize(1);
  lb.setTextDatum(TL_DATUM);
  lb.setTextColor(0xBDF7);
  lb.drawString(text, tx, ty);
  printf("=== 期望墨迹图 \"%s\" @(%d,%d) ===\n", text, tx, ty);
  for (int y = y0; y < y1; y++) {
    printf("%4d| ", y);
    for (int x = x0; x < x1; x++) putchar(buf[y * 320 + x] ? '#' : '.');
    printf("\n");
  }
}

// 墨迹图：把区域里"不是卡片底色"的像素画成 #，用来精确对齐文字位置
uint16_t simCardBg(void);
void simInkMap(const char *title, int x0, int y0, int x1, int y1) {
  uint16_t bg = simCardBg();
  printf("=== %s  (x %d..%d, y %d..%d) 墨迹图 ===\n", title, x0, x1 - 1, y0, y1 - 1);
  printf("      ");
  for (int x = x0; x < x1; x++) printf("%c", (x % 10 == 0) ? ('0' + (x / 10) % 10) : ' ');
  printf("\n      ");
  for (int x = x0; x < x1; x++) printf("%d", x % 10);
  printf("\n");
  for (int y = y0; y < y1; y++) {
    printf("%4d| ", y);
    for (int x = x0; x < x1; x++) putchar(SCREEN[y * 320 + x] == bg ? '.' : '#');
    printf("\n");
  }
}

// 某矩形内"最靠左"的非背景像素的 x（-1 表示没有）——用来区分 "0.0" 和 "--"
int simLeftmostNonBg(int x0, int y0, int x1, int y1, uint16_t bg) {
  for (int x = x0; x < x1; x++)
    for (int y = y0; y < y1; y++)
      if (SCREEN[y * 320 + x] != bg) return x;
  return -1;
}

// 某颜色在指定行/列上的跨度（验证 sprite 推送落点）
void simRunRow(const char *name, int y, uint16_t color) {
  int x0 = -1, x1 = -1, n = 0;
  for (int x = 0; x < 320; x++)
    if (SCREEN[y * 320 + x] == color) { n++; if (x0 < 0) x0 = x; x1 = x; }
  printf("  %-18s 行y=%3d  像素=%4d  x %d..%d\n", name, y, n, x0, x1);
}
void simRunCol(const char *name, int x, uint16_t color) {
  int y0 = -1, y1 = -1, n = 0;
  for (int y = 0; y < 240; y++)
    if (SCREEN[y * 320 + x] == color) { n++; if (y0 < 0) y0 = y; y1 = y; }
  printf("  %-18s 列x=%3d  像素=%4d  y %d..%d\n", name, x, n, y0, y1);
}

// 卡片底色：从一个必定是卡片内部的点采样，这样换配色不用改脚本
// （(100,40) 在三个页面里都落在卡片内部、且没有文字）
uint16_t simCardBg(void) { return SCREEN[40 * 320 + 100]; }

// 标签"逐像素完好"校验：把期望的标签单独渲染到一块空白缓冲，再和实际屏幕比。
// 期望是墨点 -> 实际必须是同色墨点；期望是空白 -> 实际必须是卡片底色。
// 这样能证明"数值没有压到标签上"（比颜色统计更严格）。
static uint16_t *g_labelBuf = nullptr;
// rows 可指定：字体 1 的字形只有 8 行，默认 10 行是为了带 2 行余量；
// 但贴着进度条/下边框的文字必须用 8 行，否则会把条槽算成"压字"。
// color 可指定：行标签是 C_LABEL(0xBDF7)，副标题是 C_LABEL_DIM(0xD69A)。
int simTextIntact(const char *text, int x, int y, int rows, uint16_t color) {
  static TFT_eSPI lb;
  static bool inited = false;
  if (!g_labelBuf) {
    g_labelBuf = (uint16_t *)calloc(320 * 240, 2);
    lb._bind(g_labelBuf, 320, 240);
    inited = true;
  }
  if (!inited) return -1;
  memset(g_labelBuf, 0, 320 * 240 * 2);
  lb.setTextFont(1);
  lb.setTextSize(1);
  lb.setTextDatum(TL_DATUM);
  lb.setTextColor(color);
  lb.drawString(text, x, y);

  uint16_t bg = simCardBg();
  int w = (int)strlen(text) * 6;
  int bad = 0;
  for (int j = 0; j < rows; j++)
    for (int i = 0; i < w; i++) {
      uint16_t want = g_labelBuf[(y + j) * 320 + (x + i)];
      uint16_t got  = SCREEN[(y + j) * 320 + (x + i)];
      if (want == color) { if (got != color) bad++; }
      else               { if (got != bg) bad++; }
    }
  return bad;
}

int simLabelIntactN(const char *text, int x, int y, int rows) {
  return simTextIntact(text, x, y, rows, 0xBDF7);   // C_LABEL
}

int simLabelIntact(const char *text, int x, int y) {
  return simLabelIntactN(text, x, y, 10);
}

// ---------------- 导出 BMP ----------------
void simSaveBmp(const char *path) {
  const int W = 320, H = 240;
  int rowBytes = (W * 3 + 3) & ~3;
  int dataSize = rowBytes * H;
  unsigned char *buf = (unsigned char *)calloc(1, 54 + dataSize);

  buf[0] = 'B'; buf[1] = 'M';
  *(int *)(buf + 2) = 54 + dataSize;
  *(int *)(buf + 10) = 54;
  *(int *)(buf + 14) = 40;
  *(int *)(buf + 18) = W;
  *(int *)(buf + 22) = H;
  *(short *)(buf + 26) = 1;
  *(short *)(buf + 28) = 24;
  *(int *)(buf + 34) = dataSize;

  for (int y = 0; y < H; y++) {
    unsigned char *row = buf + 54 + (H - 1 - y) * rowBytes;
    for (int x = 0; x < W; x++) {
      uint16_t c = SCREEN[y * W + x];
      row[x * 3 + 0] = (unsigned char)((c & 0x1F) * 255 / 31);          // B
      row[x * 3 + 1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);    // G
      row[x * 3 + 2] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);   // R
    }
  }
  FILE *f = fopen(path, "wb");
  if (!f) { printf("cannot write %s\n", path); free(buf); return; }
  fwrite(buf, 1, 54 + dataSize, f);
  fclose(f);
  free(buf);
  printf("saved %s\n", path);
}
