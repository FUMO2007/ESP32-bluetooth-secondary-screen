// ============================================================================
//  screen_draw.cpp —— 界面绘制（四页自动轮播：概览 / 散热 / 趋势 / IO·网络）
// ----------------------------------------------------------------------------
//  六条约定：
//   1) 坐标全部来自下面"布局"一节，别处不写魔法数字；擦除矩形由这些常量推导，
//      并用 static_assert 在编译期挡住溢出。
//   2) 颜色分两套语义：品牌色 accent（CPU 绿 / GPU 橙）只做标识，
//      状态色 state（绿 / 黄 / 红）只表达告警，两者不混用。
//   3) 三页共用同一套卡片几何与绘制函数，切页只换内容；切页**不清屏**，
//      靠实心卡片互相覆盖，避免整屏闪一下（切页前补清圆角缺口与卡片缝隙）。
//   4) 会变的文字一律"先画进离屏画布，再一次性推送"（regionBegin/regionEnd）。
//      先擦后画会在屏幕上留下 1~2ms 的空白，1~5Hz 连续刷新就是肉眼可见的频闪。
//   5) 告警不做数字闪烁：数值常红 + 卡片边框呼吸，避免整屏闪光。
//   6) 波形推送有最小间隔（TREND_MIN_MS）：一次推送是逐行写入，太密会看成"从上往下刷"。
//   7) 告警只"跳页提醒"ALARM_PIN_MS，不永久霸占画面：超时后照常轮播，
//      红色数值 + 边框呼吸仍在，既不漏看告警也不会把四页轮播废掉。
//
//  数据分工（每页尽量不重复）：
//    概览：负载% / 频率 / 功耗 / 显存 / 自开机最高负载 / 迷你趋势 + 峰值 / RAM·DISK·FPS
//    散热：CPU·GPU 温度大字 + 温度对比条 / 风扇转速 / 距告警余量 / 热阻 °C/W / UP·PKTS·RATE
//    趋势：整页大波形 / 窗口峰值·均值 / 帧率 当前·均值·最低
// ============================================================================
#include <Arduino.h>
#include "screen_draw.h"
#include "ble_service.h"
#include "proto.h"      // PROTO_NAME_MAX（型号名长度）

TFT_eSPI tft = TFT_eSPI();

// ===================== 主题色 =====================
#define C_BG         0x0000   // 背景
#define C_TITLE1     0x18E3   // 标题栏渐变（上）
#define C_TITLE2     0x0821   // 标题栏渐变（下）
#define C_PILL_BG    0x10A2   // 标题栏右侧链路胶囊底色
#define C_CARD       0x18E3   // 卡片背景：中性深灰 RGB(24,28,24)
                              //   原来是 0x0C63，名字写着"深蓝灰"，实际是偏亮的绿 RGB(8,142,12)：
                              //   白字压上去只有 2.3:1 对比度，小字号（刻度、副标题）根本看不清
#define C_CARD_ED    0x2965   // 卡片描边 / 内部分隔线
#define C_LABEL      0xBDF7   // 行标签、单位
#define C_LABEL_DIM  0xD69A   // 次要文字、图表刻度：中性亮灰 RGB(214,210,214)
                              //   原来是 0x7BCF（偏绿的浅色），压在卡片上对比度不到 2:1
#define C_VALUE      0xFFFF   // 数值
#define C_VALUE_DIM  0x8410   // 数据过期时的数值（置灰）
#define C_CPU        0x7FE0   // 品牌色：CPU
#define C_CPU_DIM    0x0460
#define C_GPU        0xFB00   // 品牌色：GPU
#define C_GPU_DIM    0x8800
#define C_CYAN       0x07FF   // 功率
#define C_ORANGE     0xFD20
#define C_OK         0x07E0   // 状态色
#define C_WARN       0xFFE0
#define C_ALERT      0xF800
#define C_BAR_BG     0x0841   // 进度条底槽
#define C_PLOT_BG    0x0861   // 波形绘制区底
#define C_GRID       0x2945   // 波形参考线
#define C_DOT_OFF    0x4208   // 页码圆点（未选中）

// GLCD 字体（font 1）里第 0xF8 个字形就是度符号（Fonts/glcdfont.c）。
// 注意 TFT_eSPI 默认 _cp437=false 时会把 >0xAF 的字符整体错位一格（TFT_eSPI.cpp:3204），
// 所以 screenInit() 里要显式打开 CP437 开关，0xF8 才真的落在度符号上。
#define DEG "\xF8"

// ===================== 布局（320x240 横屏，唯一一处尺寸定义）=====================
#define SCR_W        320
#define SCR_H        240
#define PAD          4                       // 屏幕边距
#define GAP          4                       // 卡片之间的缝隙

#define TITLE_H      22                      // 标题栏

#define CARD_Y       (TITLE_H + GAP)         // 26
#define CARD_H       112
#define CARD_W       154
#define CPU_X        PAD                     // 4
#define GPU_X        (CPU_X + CARD_W + GAP)  // 162

#define CARD_PAD     8                       // 卡片内边距
#define RADIUS       8
#define LBL_W        26                      // 4 字符标签的列宽
#define LBL_W_ERASE  34                      // 数值区从标签右边 34px 才开始擦：
                                             // WRITE/TOTAL/VCORE 这类 5 字符标签会写到 x+38，
                                             // 按 26px 擦会把标签尾巴擦掉（仿真抓到过）

// 卡片内部纵向偏移（相对卡片左上角）
#define TITLE_DY     3
#define SEP_DY       19
#define HERO_DY      22                      // 主指标大字
#define HERO_H       26                      // Font4 行高
#define BAR_DY       49                      // 主指标进度条
#define BAR_H        6
#define ROW_DY(i)    (58 + (i) * 18)         // 三行次要指标：58 / 76 / 94
#define ROW_H        16                      // GLCD size2 行高

// 中块：三页各放不同内容，位置一致
#define MID_Y        (CARD_Y + CARD_H + GAP) // 142
#define MID_H        62
#define MID_X        PAD
#define MID_W        (SCR_W - 2 * PAD)       // 312

// 左侧刻度栏宽度（趋势页用）
#define GUTTER_W     22
#define PLOT_X       (MID_X + GUTTER_W)         // 26
#define PLOT_W       (MID_W - GUTTER_W - PAD)   // 286：右边留 4px，别盖住卡片右边框

// 概览页：自开机极值卡片（一行两个数值，和趋势页页脚同一种排法）
#define PK_Y         MID_Y
#define PK_H         MID_H
#define PK_HEAD_Y    (MID_Y + 5)              // 147 PEAK SINCE BOOT
#define PK_ROW_Y(i)  (MID_Y + 16 + (i) * 24)  // 158 / 182  LOAD / TEMP
#define PK_LBL_X     (CPU_X + CARD_PAD)       // 12
#define PK_VAL_X     46                       // 数值槽起点
#define PK_CPU_R     152                      // CPU 数值右边界
#define PK_GPU_R     (GPU_X + CARD_W - CARD_PAD)   // 307 GPU 数值右边界

// 散热页：三条温度对比（CPU / GPU / DISK）
#define MET_Y         MID_Y
#define MET_H         MID_H
#define MET_ROW_Y(i)  (MET_Y + 3 + (i) * 18) // 145 / 163 / 181
#define MET_ROW_H     18
#define MET_LBL_X     (CPU_X + CARD_PAD)     // 12
#define MET_BAR_X     44
#define MET_BAR_W     166                    // 44..209
#define MET_BAR_H     8
#define MET_BAR_DY    5                      // 条在 18px 行里居中
#define MET_VAL_X     216                    // 数值槽起点（条右侧留 7px）
#define MET_VAL_R     (GPU_X + CARD_W - CARD_PAD)   // 307

// 趋势页：整页大波形 + 页脚两行
#define TREND_CARD_Y  CARD_Y                 // 26
#define TREND_CARD_H  178                    // 26..203
#define TREND_PLOT_X  PLOT_X                 // 26
#define TREND_PLOT_Y  (TREND_CARD_Y + 16)    // 42
#define TREND_PLOT_W  PLOT_W                 // 286（与 sprite 等宽时走快路径）
#define TREND_PLOT_H  112                    // 42..153
#define TREND_DIV_Y   (TREND_CARD_Y + 132)   // 158 页脚分隔线
#define TREND_FOOT_Y0 (TREND_CARD_Y + 136)   // 162 PEAK
#define TREND_FOOT_Y1 (TREND_CARD_Y + 155)   // 181 AVG
#define TREND_VAL_X   46                     // 页脚数值槽起点（标签列右边）
#define TREND_CPU_R   152                    // 页脚 CPU 数值右边界
#define TREND_GPU_R   (GPU_X + CARD_W - CARD_PAD)   // 307 页脚 GPU 数值右边界

// 底部状态条（三页共用位置，内容按页不同）
#define BOT_Y        (MID_Y + MID_H + GAP)   // 208
#define BOT_H        28
#define BOT_X        PAD
#define BOT_W        (SCR_W - 2 * PAD)
#define BOT_CELL     (BOT_W / 3)             // 104

// 标题栏：页码圆点
#define DOT_X        162
#define DOT_DX       16
#define DOT_Y        11

// 温度条量程与告警余量
#define TEMP_BAR_MIN 30.0f
#define TEMP_BAR_MAX 100.0f
#define TEMP_WARN    75.0f                   // 到这条线，条变黄（真正告警另有阈值）

// 编译期自检：任何一处尺寸改动越界，这里直接编译不过
static_assert(CARD_Y + CARD_H + GAP + MID_H + GAP + BOT_H + PAD <= SCR_H, "纵向布局超出屏幕");
static_assert(ROW_DY(2) + ROW_H <= CARD_H, "卡片行溢出卡片");
static_assert(HERO_DY + HERO_H <= BAR_DY, "主指标大字与进度条重叠");
static_assert(TITLE_DY + 16 <= SEP_DY, "卡片标题与分隔线重叠");
static_assert(PLOT_X + PLOT_W + PAD <= MID_X + MID_W, "波形绘制区压到卡片右边框");
static_assert(PK_ROW_Y(1) + ROW_H <= PK_Y + PK_H - PAD, "自开机极值卡片内容溢出");
static_assert(PK_VAL_X + 12 <= PK_CPU_R, "自开机极值数值槽太窄");
static_assert(MET_ROW_Y(2) + MET_ROW_H <= MET_Y + MET_H - PAD, "温度对比行溢出卡片");
static_assert(MET_BAR_X + MET_BAR_W < MET_VAL_X, "温度对比条与数值重叠");
static_assert(MET_VAL_X + 12 <= MET_VAL_R, "温度对比数值槽太窄");
static_assert(TREND_PLOT_Y + TREND_PLOT_H <= TREND_DIV_Y, "波形页绘制区压到页脚");
static_assert(TREND_FOOT_Y1 + ROW_H <= TREND_CARD_Y + TREND_CARD_H - PAD, "波形页页脚溢出卡片");
static_assert(TREND_CPU_R + 8 < TREND_GPU_R, "波形页页脚两列挨太近");
static_assert(3 * BOT_CELL + BOT_X + PAD <= SCR_W, "底部状态条超出屏幕");
static_assert(GPU_X + CARD_W + PAD <= SCR_W, "右侧卡片超出屏幕");
static_assert(DOT_X + 2 * DOT_DX + 4 < 250, "页码圆点撞到链路胶囊");
// 离屏画布必须装得下所有"会变的区域"
static_assert(MET_VAL_R - MET_BAR_X <= PLOT_W, "温度对比行比离屏画布宽");
static_assert(TREND_GPU_R - TREND_VAL_X <= PLOT_W, "页脚数值区比离屏画布宽");
static_assert(MID_H + MET_ROW_H <= TREND_PLOT_H, "离屏画布高度不够");

// ===================== 页面 =====================
enum { P_OVERVIEW = 0, P_THERMAL = 1, P_TREND = 2, P_IO = 3, PAGE_COUNT = 4 };
static const char *const PAGE_NAME[PAGE_COUNT] = {"OVERVIEW", "THERMAL", "TREND", "IO/NET"};

// 速率类进度条的量程（只影响条的长度，数字是真实值）
#define DISK_BAR_MAX   300.0f    // 磁盘吞吐满格 = 300 MB/s
#define NET_BAR_MAX    125.0f    // 网络满格 = 125 MB/s（1 Gbps 网口）
#define FAN_BAR_MAX   3000.0f    // 机箱风扇满格 = 3000 RPM
#define PING_BAR_MAX   200.0f    // 延迟满格 = 200 ms

// ===================== 运行状态 =====================
static HardwareData lastData;
static bool firstRun = true;
static bool lastStale = false;
static bool lastBle = false;
static uint8_t curPage = P_OVERVIEW;
static uint32_t pageStartMs = 0;
static uint32_t lastTick10s = 0;           // 10 秒节拍（概览页底栏：运行时长/包数/采样率）
static uint32_t lastTick1s  = 0;           // 1 秒节拍（IO 页中块：PKTS/PING/运行时长）
                                           // 两个变量分开存：以前共用一个，IO 页按 /1000 比较
                                           // 而值是按 /10000 存的，条件恒真 -> 每帧重画
static uint8_t rotIdx = 0;                 // 行轮播序号（0/1/2）
static uint8_t lastRotIdx = 0xFF;
static uint32_t rotStartMs = 0;

// 型号名（名称包到达后刷新卡片副标题）
static char lastNameCpu[PROTO_NAME_MAX] = {0};
static char lastNameGpu[PROTO_NAME_MAX] = {0};

static uint16_t lastSeenWaveIdx = 0xFFFF;  // 趋势重绘判断
static bool lastTrendStale = false;
static uint32_t lastTrendPushMs = 0;       // 波形推送限速
static uint16_t waveIdxForSample = 0xFFFF;
static uint32_t lastPushMs = 0;
static uint32_t sampleMsEma = 0;           // 实测采样间隔（波形每推进一个点的间隔）

// 自开机极值（概览页中块那张卡片用）
static float maxCpuLoad = 0.0f;
static float maxGpuLoad = 0.0f;
static float maxCpuTemp = 0.0f;
static float maxGpuTemp = 0.0f;
static float shownMaxCpu = -1.0f;          // 已经画到屏上的极值（用于判断要不要重画）
static float shownMaxGpu = -1.0f;
static float shownMaxCpuT = -1.0f;
static float shownMaxGpuT = -1.0f;

// 告警标志（带回差）
static bool alarmCpuTemp = false;
static bool alarmGpuTemp = false;
static bool alarmGpuHot  = false;
static bool alarmVramTemp = false;
static bool alarmHddTemp = false;
static bool alarmCpuLoad = false;
static bool alarmGpuLoad = false;
static bool alarmMem     = false;

// 离屏画布：既当趋势图画布（286x112 = 64KB），也当所有"局部刷新"的临时画布
static TFT_eSprite canvas = TFT_eSprite(&tft);

// 一帧内的公共状态
struct FrameCtx {
  HardwareData d;
  uint32_t now;
  bool hasData;
  bool stale;
  bool full;          // 本帧需要整体重画（首帧 / 切页 / 链路状态翻转）
  bool blinkOn;
  bool blinkFlip;
  uint8_t rot;        // 当前轮播档位
  bool rotChanged;    // 轮播档位变了（或首帧/切页），这一帧要重画轮播行
  bool aCpuT, aGpuT, aHddT, aHot, aVram, aCpuL, aGpuL, aMem;
  uint16_t cPwr, cClk, cVram;
};

// ===================== 局部刷新（治频闪的核心）=====================
// 画布可用时：先在内存里画好这一小块，再一次性推送 -> 屏幕上没有"擦干净但还没画"的空窗。
// 画布不可用（分配失败）时退化成"直接擦 + 直接画"，功能不变，只是会有轻微闪烁。
struct Canvas {
  TFT_eSPI *g;
  int dx, dy;          // 绘制偏移：把绝对坐标换算到画布局部坐标
  bool spr;
};

static Canvas regionBegin(int rx, int ry, int w, int h, uint16_t bg) {
  Canvas c;
  if (canvas.created() && w <= canvas.width() && h <= canvas.height()) {
    canvas.fillRect(0, 0, w, h, bg);
    c.g = &canvas; c.dx = -rx; c.dy = -ry; c.spr = true;
  } else {
    tft.fillRect(rx, ry, w, h, bg);
    c.g = &tft; c.dx = 0; c.dy = 0; c.spr = false;
  }
  return c;
}

static void regionEnd(const Canvas &c, int rx, int ry, int w, int h) {
  if (c.spr) canvas.pushSprite(rx, ry, 0, 0, w, h);
}

// ===================== 小工具 =====================
// 读数格式化的两条规则（协议里 0xFFFF 是"读不到"哨兵，解析后是 -1）：
//   fNum  —— 用于"0 不可能出现"的量（温度/功耗/频率）：0 或负数都当读不到
//   fZero —— 用于"0 是合法值"的量（速率/数量/百分比/风扇转速）：只有 -1 才算读不到
// 风扇必须走 fZero：0 RPM 是零转模式下的真实状态，不是没数据。
static String fNum(float v, int dec, bool hasData) {
  if (!hasData || v <= 0) return "--";
  return String(v, dec);
}

static String fZero(float v, int dec, bool hasData) {
  if (!hasData || v < 0) return "--";
  return String(v, dec);
}

// 速率（磁盘和网络共用，单位统一 MB/s / KB/s）：
//   >= 1 MB/s 用 MB/s（1 位小数），< 1 MB/s 换成 KB/s（整数）
// 空闲时网络只有几~几十 KB/s，写成 "0.0 MB/s" 就是一排 0，看着像没数据；
// 协议里网络字段是 x100（一格 0.01 MB/s = 10 KB/s），磁盘是 x10（一格约 102 KB/s）。
// 注意和任务管理器口径不同：它显示 Mbps（兆比特），1 MB/s = 8 Mbps，数字差 8 倍。
static bool rateSmall(float mbs) { return mbs >= 0.0f && mbs < 0.9995f; }

static String fMbs(float mbs, bool hasData) {
  if (!hasData || mbs < 0) return "--";
  if (rateSmall(mbs)) return String(mbs * 1024.0f, 0);
  return String(mbs, 1);
}

static const char *mbsUnit(float mbs, bool hasData) {
  if (!hasData || mbs < 0) return "MB/s";
  return rateSmall(mbs) ? "KB/s" : "MB/s";
}

static String fPct(float v, bool hasData) {
  if (!hasData || v < 0) return "--";
  return String(v, 0);
}

static String fVram(float used, float total, bool hasData) {
  if (!hasData || used < 0 || total <= 0) return "--";
  return String(used, 1) + "/" + String(total, 0);
}

static String fFps(float fps, bool hasData) {
  if (!hasData || fps <= 0) return "--";
  return String(fps, 0);
}

// 运行时长：紧凑到 4 字符以内（12m / 3h07 / 2d05），避免撑爆底栏格子
static String fUptime(uint32_t ms) {
  uint32_t s = ms / 1000;
  char b[16];
  if (s < 3600)       snprintf(b, sizeof(b), "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(b, sizeof(b), "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
  else                snprintf(b, sizeof(b), "%lud%02lu", (unsigned long)(s / 86400), (unsigned long)((s % 86400) / 3600));
  return String(b);
}

static String fPkts(uint32_t n) {
  if (n >= 10000UL) return String(n / 1000.0f, 1) + "k";
  return String(n);
}

static String fRate() {
  if (!sampleMsEma) return "--";
  return String(1000.0f / sampleMsEma, 1) + "Hz";
}

static String fWindow() {
  if (!sampleMsEma || waveCount < 2) return "--";
  uint32_t s = (uint32_t)waveCount * sampleMsEma / 1000;
  if (s < 60) return String(s) + "s";
  char b[16];
  snprintf(b, sizeof(b), "%lum%02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return String(b);
}

static uint16_t fpsColor(float fps, bool hasData, bool stale) {
  if (stale || !hasData || fps <= 0) return C_VALUE_DIM;
  if (fps >= 60) return C_OK;
  if (fps >= 30) return C_WARN;
  return C_ALERT;
}

static uint16_t stateColor(float pct, uint16_t accent) {
  if (pct >= BAR_CRIT_PCT) return C_ALERT;
  if (pct >= BAR_WARN_PCT) return C_WARN;
  return accent;
}

static float tempPct(float t) {
  float p = (t - TEMP_BAR_MIN) * 100.0f / (TEMP_BAR_MAX - TEMP_BAR_MIN);
  return constrain(p, 0.0f, 100.0f);
}

// 温度条颜色：告警红 > 偏热黄 > 品牌色
static uint16_t tempColor(float t, bool alarm, uint16_t accent) {
  if (alarm) return C_ALERT;
  if (t >= TEMP_WARN) return C_WARN;
  return accent;
}

static bool alarmUpdate(bool &flag, float v, float on) {
  if (!flag) {
    if (v >= on) flag = true;
  } else if (v <= on - ALARM_HYS) {
    flag = false;
  }
  return flag;
}

// ===================== 基础绘制（都带 dx/dy 偏移，可直接画进画布）=====================
static void drawValueRight(TFT_eSPI &g, int dx, int dy, int xRight, int y,
                           const String &main, const char *unit, uint16_t color, int slotW) {
  bool hasUnit = (unit != nullptr && unit[0] != '\0');
  if (main == "--") hasUnit = false;

  g.setTextFont(1);
  g.setTextSize(1);
  int uw  = hasUnit ? g.textWidth(String(unit), 1) : 0;
  int gap = hasUnit ? 2 : 0;

  g.setTextSize(2);
  bool fits = (g.textWidth(main, 1) + uw + gap) <= slotW;
  if (!fits) g.setTextSize(1);

  int my = fits ? y : y + 4;
  g.setTextDatum(TR_DATUM);
  g.setTextColor(color);
  g.drawString(main, dx + xRight - uw - gap, dy + my);

  if (hasUnit) {
    g.setTextSize(1);
    g.setTextColor(C_LABEL);
    g.drawString(unit, dx + xRight, dy + my + (fits ? 8 : 0));
  }
}

// 进度条（含底槽）：整条重画，所以调用方不必先擦
static void drawBar(TFT_eSPI &g, int dx, int dy, int x, int y, int w, int h,
                    float pct, uint16_t color, int thrX = -1) {
  float p = constrain(pct, 0.0f, 100.0f);
  int len = (int)(p * w / 100.0f + 0.5f);

  g.fillRoundRect(dx + x, dy + y, w, h, h / 2, C_BAR_BG);
  if (len > 1) {
    if (len >= h) g.fillRoundRect(dx + x, dy + y, len, h, h / 2, color);
    else          g.fillRect(dx + x, dy + y, len, h, color);
  }
  if (thrX >= x && thrX < x + w) {                       // 告警阈值刻度
    g.drawFastVLine(dx + thrX, dy + y, h, C_VALUE);
  }
}

// ===================== 波形取数（先声明，区域刷新里要用）=====================
// 环形缓冲按时间序取点（旧代码把 buf[0..waveIdx-1] 当线性历史，写满一圈就会清屏重画）
static int waveIndexAt(int i, int n);
static int waveCountValid();

// ===================== 区域刷新包装（对外只暴露这些）=====================
// 卡片次要行（标签固定）：区域 = 数值槽
static void updRow(int x, int row, const String &main, const char *unit, uint16_t color) {
  const int rx = x + CARD_PAD + LBL_W_ERASE;
  const int ry = CARD_Y + ROW_DY(row);
  const int w  = CARD_W - 2 * CARD_PAD - LBL_W_ERASE;
  Canvas c = regionBegin(rx, ry, w, ROW_H, C_CARD);
  drawValueRight(*c.g, c.dx, c.dy, x + CARD_W - CARD_PAD, ry, main, unit, color, w);
  regionEnd(c, rx, ry, w, ROW_H);
}

// 卡片次要行（标签会轮播变化）：区域含标签列
static void updRowLabeled(int x, int row, const char *label, const String &main,
                          const char *unit, uint16_t color) {
  const int rx = x + CARD_PAD;
  const int ry = CARD_Y + ROW_DY(row);
  const int w  = CARD_W - 2 * CARD_PAD;
  Canvas c = regionBegin(rx, ry, w, ROW_H, C_CARD);
  TFT_eSPI &g = *c.g;
  g.setTextFont(1);
  g.setTextSize(1);
  g.setTextDatum(TL_DATUM);
  g.setTextColor(C_LABEL);
  g.drawString(label, c.dx + rx, c.dy + ry + 4);
  drawValueRight(g, c.dx, c.dy, x + CARD_W - CARD_PAD, ry, main, unit, color, w - LBL_W);
  regionEnd(c, rx, ry, w, ROW_H);
}

// 卡片副标题（型号名到达后刷新）
static void updSubtitle(int x, const char *text) {
  const int rx = x + CARD_PAD + 32;
  const int ry = CARD_Y + TITLE_DY + 2;
  const int w  = CARD_W - CARD_PAD - 32;
  Canvas c = regionBegin(rx, ry, w, 13, C_CARD);
  TFT_eSPI &g = *c.g;
  g.setTextFont(1);
  g.setTextSize(1);
  g.setTextDatum(TL_DATUM);
  g.setTextColor(C_LABEL_DIM);
  g.drawString(text, c.dx + rx, c.dy + ry + 3);
  regionEnd(c, rx, ry, w, 13);
}

// 卡片主指标：大字 + 单位 + 进度条，整块一起刷
static void updHero(int x, const char *label, const String &main, const char *unit,
                    uint16_t color, float pct, uint16_t barCol) {
  const int rx = x + CARD_PAD;
  const int ry = CARD_Y + HERO_DY;
  const int w  = CARD_W - 2 * CARD_PAD;
  const int h  = (BAR_DY + BAR_H) - HERO_DY;

  Canvas c = regionBegin(rx, ry, w, h, C_CARD);
  TFT_eSPI &g = *c.g;
  const int dx = c.dx, dy = c.dy;
  const int xr = x + CARD_W - CARD_PAD;

  g.setTextFont(1);
  g.setTextSize(1);
  g.setTextDatum(TL_DATUM);
  g.setTextColor(C_LABEL);
  g.drawString(label, dx + rx, dy + ry + 10);

  int uw = g.textWidth(String(unit), 1);
  int gap = (main == "--") ? 0 : 2;
  if (main == "--") uw = 0;

  g.setTextFont(4);                       // 26px 高，视觉主角
  g.setTextSize(1);
  g.setTextDatum(TR_DATUM);
  g.setTextColor(color);
  g.drawString(main, dx + xr - uw - gap, dy + ry);

  if (uw) {
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextColor(C_LABEL);
    g.drawString(unit, dx + xr, dy + ry + HERO_H - 8);
  }

  g.setTextFont(1);
  g.setTextSize(1);
  drawBar(g, dx, dy, rx, CARD_Y + BAR_DY, w, BAR_H, (main == "--") ? 0 : pct, barCol);
  regionEnd(c, rx, ry, w, h);
}

// 底部状态条的一格
static void updCell(int i, const String &main, const char *unit, uint16_t color) {
  const int x0 = BOT_X + i * BOT_CELL;
  const int y  = BOT_Y + (BOT_H - ROW_H) / 2;
  const int rx = x0 + 30;
  const int w  = BOT_CELL - 30 - CARD_PAD;

  Canvas c = regionBegin(rx, y, w, ROW_H, C_CARD);
  drawValueRight(*c.g, c.dx, c.dy, x0 + BOT_CELL - CARD_PAD, y, main, unit, color, w);
  regionEnd(c, rx, y, w, ROW_H);
}

// 散热页：一条温度对比（标签静态，条 + 数值一起刷）
static void updMeter(int row, float temp, float alarmTemp, uint16_t accent, bool alarm, bool hasData) {
  const int ry = MET_ROW_Y(row);
  const int rx = MET_BAR_X;
  const int w  = MET_VAL_R - MET_BAR_X;

  Canvas c = regionBegin(rx, ry, w, MET_ROW_H, C_CARD);
  TFT_eSPI &g = *c.g;
  const int dx = c.dx, dy = c.dy;

  uint16_t col = hasData ? tempColor(temp, alarm, accent) : C_BAR_BG;
  float p = hasData ? tempPct(temp) : 0.0f;

  // 告警阈值刻度位置（同一个量程里换算）
  int thrX = -1;
  if (alarmTemp > TEMP_BAR_MIN && alarmTemp < TEMP_BAR_MAX) {
    thrX = MET_BAR_X + (int)((alarmTemp - TEMP_BAR_MIN) * MET_BAR_W / (TEMP_BAR_MAX - TEMP_BAR_MIN));
  }
  drawBar(g, dx, dy, MET_BAR_X, ry + MET_BAR_DY, MET_BAR_W, MET_BAR_H, p, col, thrX);

  drawValueRight(g, dx, dy, MET_VAL_R, ry, fNum(temp, 1, hasData), DEG "C",
                 hasData ? (alarm ? C_ALERT : C_VALUE) : C_VALUE_DIM,
                 MET_VAL_R - MET_VAL_X);
  regionEnd(c, rx, ry, w, MET_ROW_H);
}

// 通用仪表行：按给定 pct(0..100) 画条 + 右侧数值（用于风扇/延迟这类非温度量）
static void updGauge(int row, float pct, const String &val, const char *unit,
                     uint16_t color, bool hasData) {
  const int ry = MET_ROW_Y(row);
  const int rx = MET_BAR_X;
  const int w  = MET_VAL_R - MET_BAR_X;

  Canvas c = regionBegin(rx, ry, w, MET_ROW_H, C_CARD);
  TFT_eSPI &g = *c.g;
  drawBar(g, c.dx, c.dy, MET_BAR_X, ry + MET_BAR_DY, MET_BAR_W, MET_BAR_H,
          hasData ? pct : 0.0f, hasData ? color : C_BAR_BG);
  drawValueRight(g, c.dx, c.dy, MET_VAL_R, ry, val, unit,
                 hasData ? color : C_VALUE_DIM, MET_VAL_R - MET_VAL_X);
  regionEnd(c, rx, ry, w, MET_ROW_H);
}

// 仪表行的标签（轮播时标签也跟着换，所以单独一个小区域）
static void updMeterLabel(int row, const char *label) {
  const int ry = MET_ROW_Y(row);
  Canvas c = regionBegin(MET_LBL_X, ry, 32, MET_ROW_H, C_CARD);
  TFT_eSPI &g = *c.g;
  g.setTextFont(1);
  g.setTextSize(1);
  g.setTextDatum(TL_DATUM);
  g.setTextColor(C_LABEL);
  g.drawString(label, c.dx + MET_LBL_X, c.dy + ry + 5);
  regionEnd(c, MET_LBL_X, ry, 32, MET_ROW_H);
}

// 一行两个数值（左侧 CPU / 右侧 GPU）：概览页的自开机极值和趋势页页脚共用
// lbl2 是右列的标签：必须**在这一块区域里一起重画**，不能写在该页的静态函数里
// （静态标签落在动态区域内，每次刷新都会被整块填底擦掉——这个坑踩过一次）
static void updPairRow(int y, int valX, int cpuR, int gpuR,
                       uint16_t c1, uint16_t c2, const String &s1, const String &s2,
                       const char *unit = "", const char *lbl2 = nullptr) {
  const int rx = valX;
  const int w  = gpuR - rx;

  Canvas c = regionBegin(rx, y, w, ROW_H, C_CARD);
  TFT_eSPI &g = *c.g;
  const int dx = c.dx, dy = c.dy;

  if (lbl2) {
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(C_LABEL);
    g.drawString(lbl2, dx + cpuR + 8, dy + y + 4);
  }
  drawValueRight(g, dx, dy, cpuR, y, s1, unit, c1, cpuR - rx);
  drawValueRight(g, dx, dy, gpuR, y, s2, unit, c2, gpuR - cpuR - 8);
  regionEnd(c, rx, y, w, ROW_H);
}

// 概览页中块：自开机极值卡片的一行（左右两列分别是 CPU / GPU，颜色区分）
static void updPeakRow(int i, uint16_t c1, uint16_t c2, const String &s1, const String &s2,
                       const char *unit) {
  updPairRow(PK_ROW_Y(i), PK_VAL_X, PK_CPU_R, PK_GPU_R, c1, c2, s1, s2, unit);
}

// 趋势页页脚的一行
static void updFootRow(int y, uint16_t c1, uint16_t c2, const String &s1, const String &s2) {
  updPairRow(y, TREND_VAL_X, TREND_CPU_R, TREND_GPU_R, c1, c2, s1, s2);
}

// 趋势页右上角窗口时长（长度会变，整块刷）
static void updWindow() {
  const int rx = TREND_PLOT_X + TREND_PLOT_W - 104;
  Canvas c = regionBegin(rx, TREND_CARD_Y + 3, 104, 11, C_CARD);
  TFT_eSPI &g = *c.g;
  const int dx = c.dx, dy = c.dy;

  g.setTextFont(1);
  g.setTextSize(1);
  g.setTextDatum(TR_DATUM);
  g.setTextColor(C_LABEL_DIM);
  g.drawString("WINDOW", dx + TREND_PLOT_X + TREND_PLOT_W - 48, dy + TREND_CARD_Y + 4);
  g.setTextColor(C_LABEL);
  g.drawString(fWindow(), dx + TREND_PLOT_X + TREND_PLOT_W, dy + TREND_CARD_Y + 4);
  regionEnd(c, rx, TREND_CARD_Y + 3, 104, 11);
}

// ===================== 波形 =====================
static int waveIndexAt(int i, int n) {
  int idx = (int)waveIdx - n + i;
  if (idx < 0) idx += WAVE_POINTS;
  return idx % WAVE_POINTS;
}

static int waveCountValid() {
  return (waveCount > WAVE_POINTS) ? WAVE_POINTS : waveCount;
}

// g 既可以是离屏画布，也可以是屏幕本身（兜底）
static void plotTrend(TFT_eSPI &g, int ox, int oy, int w, int h, bool stale) {
  g.fillRect(ox, oy, w, h, C_PLOT_BG);

  for (int i = 1; i <= 3; i++) {
    int gy = oy + h - 1 - (i * (h - 1) / 4);
    for (int x = 0; x < w; x += 4) {
      g.drawPixel(ox + x, gy, C_GRID);
      g.drawPixel(ox + x + 1, gy, C_GRID);
    }
  }

  if (stale) {
    g.setTextFont(1);
    g.setTextSize(1);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(C_ALERT);
    g.drawString("NO SIGNAL", ox + (w - 54) / 2, oy + h / 2 - 4);
    return;
  }

  int n = waveCountValid();
  if (n < 2) return;

  int prevX = 0, prevYc = 0, prevYg = 0;
  for (int i = 0; i < n; i++) {
    int idx = waveIndexAt(i, n);
    int x   = ox + w - 1 - (n - 1 - i) * w / WAVE_POINTS;
    int yc  = oy + h - 1 - constrain(cpuWave[idx], 0, 100) * (h - 1) / 100;
    int yg  = oy + h - 1 - constrain(gpuWave[idx], 0, 100) * (h - 1) / 100;

    if (i) {
      g.drawLine(prevX, prevYc, x, yc, C_CPU);
      g.drawLine(prevX, prevYc + 1, x, yc + 1, C_CPU);
      g.drawLine(prevX, prevYg, x, yg, C_GPU);
      g.drawLine(prevX, prevYg + 1, x, yg + 1, C_GPU);
    }
    prevX = x; prevYc = yc; prevYg = yg;
  }
  g.fillCircle(ox + w - 1, prevYc, 2, C_CPU);
  g.fillCircle(ox + w - 1, prevYg, 2, C_GPU);
}

// 波形推送：一次窗口写入（无闪烁），并限制最小间隔，避免高刷时看成"从上往下刷"。
// 返回值 = 这一帧真的推上去了。被限速跳过的帧**不算画过** —— 调用方如果照旧更新
// 游标，那一次 waveIdx/断线状态变化就被永久吞掉：链路上若恰好在限速窗口里断掉，
// waveIdx 从此不再变化，NO SIGNAL 永远画不出来（画面停在最后一帧旧曲线）。
static bool pushTrend(int x, int y, int w, int h, bool stale, bool force) {
  if (!force && (uint32_t)(millis() - lastTrendPushMs) < TREND_MIN_MS) return false;
  lastTrendPushMs = millis();

  if (canvas.created()) {
    plotTrend(canvas, 0, 0, w, h, stale);
    canvas.pushSprite(x, y, 0, 0, w, h);
  } else {
    plotTrend(tft, x, y, w, h, stale);
  }
  return true;
}

// 窗口内的帧率统计（0 视为"没在跑游戏"，不参与统计）
// 注：1% low 已改由 PC 端在逐帧数据上算好送来（协议字段 fps_low），
// 这里原来那个"取环形缓冲最小值"的函数连同 fpsWave 一起删掉了 ——
// 它是单帧极值，不是 1% low，而且它的 avg 出参从来没人用过。

// 窗口内的负载峰值/均值
static bool loadStats(int &pc, int &pg, int &ac, int &ag) {
  int n = waveCountValid();
  if (n < 2) return false;
  int sc = 0, sg = 0;
  pc = 0; pg = 0;
  for (int i = 0; i < n; i++) {
    int idx = waveIndexAt(i, n);
    int a = constrain(cpuWave[idx], 0, 100);
    int b = constrain(gpuWave[idx], 0, 100);
    sc += a; sg += b;
    if (a > pc) pc = a;
    if (b > pg) pg = b;
  }
  ac = sc / n; ag = sg / n;
  return true;
}

// ===================== 静态界面构件 =====================
static void drawTitleBar(const char *pageName, int page) {
  tft.fillRectVGradient(0, 0, SCR_W, TITLE_H, C_TITLE1, C_TITLE2);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_VALUE);
  tft.drawString("PC MONITOR", 8, 3);

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(C_LABEL_DIM);
  tft.drawString(pageName, 96, 7);

  for (int i = 0; i < PAGE_COUNT; i++) {
    int x = DOT_X + i * DOT_DX;
    if (i == page) {
      tft.fillCircle(x, DOT_Y, 3, C_VALUE);
      tft.drawCircle(x, DOT_Y, 3, C_LABEL);
    } else {
      tft.drawCircle(x, DOT_Y, 3, C_DOT_OFF);
    }
  }
  tft.drawFastHLine(0, TITLE_H - 1, SCR_W, 0x1CAF);
}

// 链路胶囊：整条标题栏重画后再画（旧写法只补画一小块渐变，接缝肉眼可见）
static void drawBlePill() {
  const int px = 250, py = 3, pw = SCR_W - px - PAD, ph = TITLE_H - 6;
  uint16_t col = bleConnected ? C_OK : C_ALERT;

  tft.fillRoundRect(px, py, pw, ph, ph / 2, C_PILL_BG);
  tft.drawRoundRect(px, py, pw, ph, ph / 2, col);
  tft.fillCircle(px + 10, py + ph / 2, 3, col);

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col);
  tft.drawString(bleConnected ? "ONLINE" : "OFFLINE", px + 18, py + 4);
}

// 卡片框（实心填充，切页时靠它覆盖上一页的内容，所以不需要清屏）
static void drawCardFrame(int x, const char *title, const char *sub,
                          uint16_t accent, uint16_t border) {
  tft.fillRoundRect(x, CARD_Y, CARD_W, CARD_H, RADIUS, C_CARD);
  tft.drawRoundRect(x, CARD_Y, CARD_W, CARD_H, RADIUS, border);

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(accent);
  tft.drawString(title, x + CARD_PAD, CARD_Y + TITLE_DY);

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(C_LABEL_DIM);
  tft.drawString(sub, x + CARD_PAD + 32, CARD_Y + TITLE_DY + 5);

  tft.drawFastHLine(x + CARD_PAD, CARD_Y + SEP_DY, CARD_W - 2 * CARD_PAD, C_CARD_ED);
}

static void drawCardBorder(int x, uint16_t color) {
  tft.drawRoundRect(x, CARD_Y, CARD_W, CARD_H, RADIUS, color);
}

static void drawRowLabel(int x, int row, const char *label) {
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_LABEL);
  tft.drawString(label, x + CARD_PAD, CARD_Y + ROW_DY(row) + 4);
}

// 中块卡片框（概览的波形卡 / 散热的对比卡 / 趋势的大图卡都走这里）
static void drawMidFrame(int y, int h, uint16_t border) {
  tft.fillRoundRect(MID_X, y, MID_W, h, RADIUS, C_CARD);
  tft.drawRoundRect(MID_X, y, MID_W, h, RADIUS, border);
}

// 切页前把"上一页画过、这一页盖不到"的地方清成背景色：
//   · 趋势页那张大卡片实心覆盖 26..203，切回卡片版式时卡片之间的缝隙没人去盖
//   · 圆角卡片是圆角填充，四个角的缺口会露出上一页的内容（深色波形底尤其明显）
// 这两处残留都是仿真的像素断言抓出来的，肉眼可能只看到"边角有点脏"。
static void clearCorners(int x, int y, int w, int h) {
  tft.fillRect(x, y, RADIUS, RADIUS, C_BG);
  tft.fillRect(x + w - RADIUS, y, RADIUS, RADIUS, C_BG);
  tft.fillRect(x, y + h - RADIUS, RADIUS, RADIUS, C_BG);
  tft.fillRect(x + w - RADIUS, y + h - RADIUS, RADIUS, RADIUS, C_BG);
}

static void clearBeforeCards() {
  clearCorners(CPU_X, CARD_Y, CARD_W, CARD_H);
  clearCorners(GPU_X, CARD_Y, CARD_W, CARD_H);
  clearCorners(MID_X, MID_Y, MID_W, MID_H);
  clearCorners(BOT_X, BOT_Y, BOT_W, BOT_H);
  tft.fillRect(0, CARD_Y + CARD_H, SCR_W, MID_Y - (CARD_Y + CARD_H), C_BG);   // y 138..141
  tft.fillRect(0, MID_Y + MID_H, SCR_W, BOT_Y - (MID_Y + MID_H), C_BG);       // y 204..207
  tft.fillRect(CPU_X + CARD_W, CARD_Y, GAP, CARD_H, C_BG);                    // x 158..161
}

static void clearBeforeTrend() {
  clearCorners(MID_X, TREND_CARD_Y, MID_W, TREND_CARD_H);
  clearCorners(BOT_X, BOT_Y, BOT_W, BOT_H);
}

static void drawStripFrame(const char *const *labels) {
  tft.fillRoundRect(BOT_X, BOT_Y, BOT_W, BOT_H, RADIUS, C_CARD);
  tft.drawRoundRect(BOT_X, BOT_Y, BOT_W, BOT_H, RADIUS, C_CARD_ED);
  tft.drawFastVLine(BOT_X + BOT_CELL,     BOT_Y + 7, BOT_H - 14, C_CARD_ED);
  tft.drawFastVLine(BOT_X + 2 * BOT_CELL, BOT_Y + 7, BOT_H - 14, C_CARD_ED);

  for (int i = 0; i < 3; i++) {
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL);
    tft.drawString(labels[i], BOT_X + i * BOT_CELL + 6, BOT_Y + (BOT_H - ROW_H) / 2 + 4);
  }
}

// ===================== 概览页（实时数值 + 自开机极值，不放曲线图）=====================
static void drawStaticOverview() {
  clearBeforeCards();
  drawTitleBar(PAGE_NAME[P_OVERVIEW], P_OVERVIEW);
  drawBlePill();

  drawCardFrame(CPU_X, "CPU", "PROCESSOR", C_CPU, C_CPU_DIM);
  drawCardFrame(GPU_X, "GPU", "GRAPHICS",  C_GPU, C_GPU_DIM);

  static const char *cpuRows[3] = {"TEMP", "CLK", "PWR"};
  drawRowLabel(CPU_X, 0, cpuRows[0]);
  drawRowLabel(CPU_X, 1, cpuRows[1]);
  drawRowLabel(GPU_X, 0, "TEMP");
  drawRowLabel(GPU_X, 1, "VRAM");
  // 第 3 行是轮播行（PWR ↔ VCORE ↔ CORE / PWR ↔ GCLK ↔ VMEM），标签由轮播逻辑画

  // 中块：自开机极值（一行负载峰值、一行温度峰值，都是本页独有的数据；曲线只在趋势页）
  drawMidFrame(PK_Y, PK_H, C_CARD_ED);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_LABEL_DIM);
  tft.drawString("PEAK SINCE BOOT", PK_LBL_X, PK_HEAD_Y);
  tft.setTextColor(C_LABEL);
  tft.drawString("LOAD", PK_LBL_X, PK_ROW_Y(0) + 4);
  tft.drawString("TEMP", PK_LBL_X, PK_ROW_Y(1) + 4);

  static const char *labels[3] = {"RAM", "DISK", "FPS"};
  drawStripFrame(labels);
}

static void pageOverview(const FrameCtx &c) {
  if (c.full || lastData.cpuLoad != c.d.cpuLoad) {
    uint16_t col = c.stale ? C_VALUE_DIM : (c.aCpuL ? C_ALERT : C_CPU);
    updHero(CPU_X, "LOAD", fPct(c.d.cpuLoad, c.hasData), "%", col,
            c.d.cpuLoad, c.stale ? C_VALUE_DIM : stateColor(c.d.cpuLoad, C_CPU));
  }
  if (c.full || lastData.cpuTemp != c.d.cpuTemp) {
    updRow(CPU_X, 0, fNum(c.d.cpuTemp, 1, c.hasData), DEG "C",
           c.stale ? C_VALUE_DIM : (c.aCpuT ? C_ALERT : C_VALUE));
  }
  if (c.full || lastData.cpuClk != c.d.cpuClk) {
    updRow(CPU_X, 1, fNum(c.d.cpuClk / 1000.0f, 1, c.hasData), "GHz", c.cClk);
  }
  // 第 3 行：功耗 ↔ 电压 ↔ 最忙核心（每 ROT_MS 换一档）
  if (c.full || c.rotChanged || lastData.cpuPower != c.d.cpuPower ||
      lastData.cpuVolt != c.d.cpuVolt || lastData.cpuCoreMax != c.d.cpuCoreMax) {
    switch (c.rot) {
      case 1:
        updRowLabeled(CPU_X, 2, "VCORE", fNum(c.d.cpuVolt, 2, c.hasData), "V",
                      c.stale ? C_VALUE_DIM : C_LABEL);
        break;
      case 2:
        updRowLabeled(CPU_X, 2, "CORE", fNum(c.d.cpuCoreMax, 0, c.hasData), "%",
                      c.stale ? C_VALUE_DIM : C_LABEL);
        break;
      default:
        updRowLabeled(CPU_X, 2, "PWR", fNum(c.d.cpuPower, 1, c.hasData), "W", c.cPwr);
        break;
    }
  }

  if (c.full || lastData.gpuLoad != c.d.gpuLoad) {
    uint16_t col = c.stale ? C_VALUE_DIM : (c.aGpuL ? C_ALERT : C_GPU);
    updHero(GPU_X, "LOAD", fPct(c.d.gpuLoad, c.hasData), "%", col,
            c.d.gpuLoad, c.stale ? C_VALUE_DIM : stateColor(c.d.gpuLoad, C_GPU));
  }
  if (c.full || lastData.gpuTemp != c.d.gpuTemp) {
    updRow(GPU_X, 0, fNum(c.d.gpuTemp, 1, c.hasData), DEG "C",
           c.stale ? C_VALUE_DIM : (c.aGpuT ? C_ALERT : C_VALUE));
  }
  if (c.full || lastData.gpuMem != c.d.gpuMem || lastData.gpuMemTotal != c.d.gpuMemTotal) {
    updRow(GPU_X, 1, fVram(c.d.gpuMem, c.d.gpuMemTotal, c.hasData), "GB", c.cVram);
  }
  // 第 3 行：功耗 ↔ GPU 频率 ↔ 显存控制器负载
  if (c.full || c.rotChanged || lastData.gpuPower != c.d.gpuPower ||
      lastData.gpuClk != c.d.gpuClk || lastData.gpuMemCtrl != c.d.gpuMemCtrl) {
    switch (c.rot) {
      case 1:
        updRowLabeled(GPU_X, 2, "GCLK", fNum(c.d.gpuClk, 0, c.hasData), "MHz",
                      c.stale ? C_VALUE_DIM : C_LABEL);
        break;
      case 2:
        updRowLabeled(GPU_X, 2, "VMEM", fNum(c.d.gpuMemCtrl, 0, c.hasData), "%",
                      c.stale ? C_VALUE_DIM : C_LABEL);
        break;
      default:
        updRowLabeled(GPU_X, 2, "PWR", fNum(c.d.gpuPower, 1, c.hasData), "W", c.cPwr);
        break;
    }
  }

  // 自开机极值：负载峰值 / 温度峰值（行内两个数值一起刷）
  if (c.full || maxCpuLoad != shownMaxCpu || maxGpuLoad != shownMaxGpu) {
    updPeakRow(0, c.stale ? C_VALUE_DIM : C_CPU, c.stale ? C_VALUE_DIM : C_GPU,
               fPct(maxCpuLoad, c.hasData), fPct(maxGpuLoad, c.hasData), "%");
    shownMaxCpu = maxCpuLoad;
    shownMaxGpu = maxGpuLoad;
  }
  if (c.full || maxCpuTemp != shownMaxCpuT || maxGpuTemp != shownMaxGpuT) {
    updPeakRow(1, c.stale ? C_VALUE_DIM : C_VALUE, c.stale ? C_VALUE_DIM : C_VALUE,
               fNum(maxCpuTemp, 1, c.hasData), fNum(maxGpuTemp, 1, c.hasData), DEG "C");
    shownMaxCpuT = maxCpuTemp;
    shownMaxGpuT = maxGpuTemp;
  }

  if (c.full || lastData.memUsed != c.d.memUsed) {
    uint16_t col = c.stale ? C_VALUE_DIM : (c.aMem ? C_ALERT : stateColor(c.d.memUsed, C_OK));
    updCell(0, fPct(c.d.memUsed, c.hasData), "%", col);
  }
  if (c.full || lastData.hddTemp != c.d.hddTemp) {
    updCell(1, fNum(c.d.hddTemp, 1, c.hasData), DEG "C",
            c.stale ? C_VALUE_DIM : (c.aHddT ? C_ALERT : C_ORANGE));
  }
  if (c.full || lastData.fps != c.d.fps) {
    updCell(2, fFps(c.d.fps, c.hasData), "", fpsColor(c.d.fps, c.hasData, c.stale));
  }
}

// ===================== 散热页（温度 / 风扇 / 余量 / 热阻）=====================
static void drawStaticThermal() {
  clearBeforeCards();
  drawTitleBar(PAGE_NAME[P_THERMAL], P_THERMAL);
  drawBlePill();

  drawCardFrame(CPU_X, "CPU", "THERMAL", C_CPU, C_CPU_DIM);
  drawCardFrame(GPU_X, "GPU", "THERMAL", C_GPU, C_GPU_DIM);

  static const char *cpuRows[3] = {"FAN", "MARG", DEG "C/W"};
  static const char *gpuRows[3] = {"FAN", "MARG", DEG "C/W"};
  for (int i = 0; i < 3; i++) {
    drawRowLabel(CPU_X, i, cpuRows[i]);
    drawRowLabel(GPU_X, i, gpuRows[i]);
  }

  drawMidFrame(MET_Y, MET_H, C_CARD_ED);
  // 三条对比的标签随轮播变化（第 A 组：CPU/GPU/DISK 温度；第 B 组：热点/显存温度/机箱风扇）
  // 所以这里不画标签，由 pageThermal() 在首帧和轮播切换时画

  static const char *labels[3] = {"UP", "PKTS", "RATE"};
  drawStripFrame(labels);
}

static void pageThermal(const FrameCtx &c) {
  // ---- CPU：温度大字 + 温度计 ----
  if (c.full || lastData.cpuTemp != c.d.cpuTemp) {
    uint16_t col = c.stale ? C_VALUE_DIM : (c.aCpuT ? C_ALERT : C_CPU);
    updHero(CPU_X, "TEMP", fNum(c.d.cpuTemp, 1, c.hasData), DEG "C", col,
            tempPct(c.d.cpuTemp), c.stale ? C_VALUE_DIM : tempColor(c.d.cpuTemp, c.aCpuT, C_CPU));
  }
  if (c.full || lastData.cpuFan != c.d.cpuFan) {
    // 风扇用 fZero：0 RPM 是合法且有意义的读数（显卡/机箱风扇的零转模式在轻载时是常态），
    // 用 fNum 会把它显示成"没数据"，恰好盖掉一个值得注意的状态
    updRow(CPU_X, 0, fZero(c.d.cpuFan, 0, c.hasData), "RPM", c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.cpuTemp != c.d.cpuTemp) {
    float marg = ALARM_CPU_TEMP - c.d.cpuTemp;
    updRow(CPU_X, 1, fNum(marg, 0, c.hasData && c.d.cpuTemp > 0), DEG "C",
           c.stale ? C_VALUE_DIM : (c.aCpuT ? C_ALERT : C_LABEL));
  }
  if (c.full || lastData.cpuTemp != c.d.cpuTemp || lastData.cpuPower != c.d.cpuPower) {
    bool ok = c.hasData && c.d.cpuTemp > 0 && c.d.cpuPower > 1.0f;
    updRow(CPU_X, 2, ok ? String(c.d.cpuTemp / c.d.cpuPower, 2) : "--", "",
           c.stale ? C_VALUE_DIM : C_LABEL);
  }

  // ---- GPU ----
  if (c.full || lastData.gpuTemp != c.d.gpuTemp) {
    uint16_t col = c.stale ? C_VALUE_DIM : (c.aGpuT ? C_ALERT : C_GPU);
    updHero(GPU_X, "TEMP", fNum(c.d.gpuTemp, 1, c.hasData), DEG "C", col,
            tempPct(c.d.gpuTemp), c.stale ? C_VALUE_DIM : tempColor(c.d.gpuTemp, c.aGpuT, C_GPU));
  }
  if (c.full || lastData.gpuFan != c.d.gpuFan) {
    // 同上：GPU 零风扇模式的 0 RPM 要显示成 0，不是 --
    updRow(GPU_X, 0, fZero(c.d.gpuFan, 0, c.hasData), "RPM", c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.gpuTemp != c.d.gpuTemp) {
    float marg = ALARM_GPU_TEMP - c.d.gpuTemp;
    updRow(GPU_X, 1, fNum(marg, 0, c.hasData && c.d.gpuTemp > 0), DEG "C",
           c.stale ? C_VALUE_DIM : (c.aGpuT ? C_ALERT : C_LABEL));
  }
  if (c.full || lastData.gpuTemp != c.d.gpuTemp || lastData.gpuPower != c.d.gpuPower) {
    bool ok = c.hasData && c.d.gpuTemp > 0 && c.d.gpuPower > 1.0f;
    updRow(GPU_X, 2, ok ? String(c.d.gpuTemp / c.d.gpuPower, 2) : "--", "",
           c.stale ? C_VALUE_DIM : C_LABEL);
  }

  // ---- 温度对比条：两组轮播（A：CPU/GPU/DISK 温度；B：GPU热点/显存温度/机箱风扇）----
  if (c.rot == 0) {
    if (c.full || c.rotChanged) {
      updMeterLabel(0, "CPU");
      updMeterLabel(1, "GPU");
      updMeterLabel(2, "DISK");
    }
    if (c.full || c.rotChanged || lastData.cpuTemp != c.d.cpuTemp) {
      updMeter(0, c.d.cpuTemp, ALARM_CPU_TEMP, C_CPU, c.aCpuT, c.hasData);
    }
    if (c.full || c.rotChanged || lastData.gpuTemp != c.d.gpuTemp) {
      updMeter(1, c.d.gpuTemp, ALARM_GPU_TEMP, C_GPU, c.aGpuT, c.hasData);
    }
    if (c.full || c.rotChanged || lastData.hddTemp != c.d.hddTemp) {
      updMeter(2, c.d.hddTemp, ALARM_HDD_TEMP, C_ORANGE, c.aHddT, c.hasData);
    }
  } else {
    if (c.full || c.rotChanged) {
      updMeterLabel(0, "HOT");
      updMeterLabel(1, "VRAM");
      updMeterLabel(2, "CHAS");
    }
    if (c.full || c.rotChanged || lastData.gpuHotspot != c.d.gpuHotspot) {
      updMeter(0, c.d.gpuHotspot, ALARM_GPU_HOT, C_GPU, c.aHot, c.hasData);
    }
    if (c.full || c.rotChanged || lastData.gpuMemTemp != c.d.gpuMemTemp) {
      updMeter(1, c.d.gpuMemTemp, ALARM_VRAM_TEMP, C_GPU, c.aVram, c.hasData);
    }
    if (c.full || c.rotChanged || lastData.moboFan != c.d.moboFan) {
      float pct = c.d.moboFan > 0 ? c.d.moboFan * 100.0f / FAN_BAR_MAX : 0.0f;
      updGauge(2, pct, fZero(c.d.moboFan, 0, c.hasData), "RPM",
               c.stale ? C_VALUE_DIM : C_LABEL, c.hasData);
    }
  }

  // ---- 底栏：系统/链路诊断（10 秒节拍）----
  bool tick = c.full || (c.now / 10000) != lastTick10s;
  if (tick) {
    updCell(0, fUptime(c.now), "", c.stale ? C_VALUE_DIM : C_LABEL);
    updCell(1, fPkts(blePacketCount()), "", c.stale ? C_VALUE_DIM : C_LABEL);
    updCell(2, fRate(), "", c.stale ? C_VALUE_DIM : C_LABEL);
  }
}

// ===================== 趋势页（历史：大波形 + 峰值均值 + 帧率统计）=====================
static void drawStaticTrend() {
  clearBeforeTrend();
  drawTitleBar(PAGE_NAME[P_TREND], P_TREND);
  drawBlePill();

  drawMidFrame(TREND_CARD_Y, TREND_CARD_H, C_CARD_ED);

  tft.fillRect(TREND_PLOT_X,      TREND_CARD_Y + 7, 8, 4, C_CPU);
  tft.fillRect(TREND_PLOT_X + 40, TREND_CARD_Y + 7, 8, 4, C_GPU);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_LABEL);
  tft.drawString("CPU", TREND_PLOT_X + 11, TREND_CARD_Y + 4);
  tft.drawString("GPU", TREND_PLOT_X + 51, TREND_CARD_Y + 4);

  tft.setTextColor(C_LABEL_DIM);
  tft.drawString("100", MID_X + 2, TREND_PLOT_Y - 2);
  tft.drawString("50",  MID_X + 2, TREND_PLOT_Y + TREND_PLOT_H / 2 - 4);
  tft.drawString("0",   MID_X + 2, TREND_PLOT_Y + TREND_PLOT_H - 6);

  tft.drawFastHLine(MID_X + CARD_PAD, TREND_DIV_Y, MID_W - 2 * CARD_PAD, C_CARD_ED);
  tft.setTextColor(C_LABEL);          // 和别处的行标签同色（这里曾漏设，画成了暗灰）
  tft.drawString("PEAK", MID_X + CARD_PAD, TREND_FOOT_Y0 + 4);
  tft.drawString("AVG",  MID_X + CARD_PAD, TREND_FOOT_Y1 + 4);

  static const char *labels[3] = {"FPS", "TIME", "LOW"};
  drawStripFrame(labels);
}

static void pageTrend(const FrameCtx &c) {
  bool waveChanged = (waveIdx != lastSeenWaveIdx) || (c.stale != lastTrendStale);
  if (c.full || waveChanged) {
    // 只有真的推上去了才更新游标：被限速跳过的帧下一轮会原样重试，
    // 不会把"波形/断线状态变了"这件事丢掉（见 pushTrend 的注释）
    if (pushTrend(TREND_PLOT_X, TREND_PLOT_Y, TREND_PLOT_W, TREND_PLOT_H, c.stale, c.full)) {
      updWindow();

      int pc, pg, ac, ag;
      bool ok = (!c.stale) && loadStats(pc, pg, ac, ag);
      updFootRow(TREND_FOOT_Y0, ok ? C_CPU : C_LABEL_DIM, ok ? C_GPU : C_LABEL_DIM,
                 ok ? String(pc) + "%" : "--", ok ? String(pg) + "%" : "--");
      updFootRow(TREND_FOOT_Y1, ok ? C_CPU : C_LABEL_DIM, ok ? C_GPU : C_LABEL_DIM,
                 ok ? String(ac) + "%" : "--", ok ? String(ag) + "%" : "--");

      lastSeenWaveIdx = waveIdx;
      lastTrendStale = c.stale;
    }
  }

  // 底栏：帧率 当前 / 帧时间 / 1% low
  // LOW 用的是 PC 端在**逐帧**数据上算好的 1% low（协议第 31 个字段 fps_low）。
  // 以前这里显示的是"140 秒环形缓冲里最差的那一个采样"——那是**单帧极值**，
  // 一颗老鼠屎会在屏幕上挂两分多钟，而且它和下面的平均值并不回答同一个问题。
  // 1% low 取"最慢的 1% 帧的平均帧率"，抗离群点，才是通用的卡顿指标。
  if (c.full || lastData.fps != c.d.fps || lastData.frameMs != c.d.frameMs
      || lastData.fpsLow != c.d.fpsLow) {
    uint16_t fcol = c.stale ? C_VALUE_DIM
                            : (c.d.fps >= 60 ? C_OK : c.d.fps >= 30 ? C_WARN : C_ALERT);
    updCell(0, fFps(c.d.fps, c.hasData), "", fpsColor(c.d.fps, c.hasData, c.stale));
    updCell(1, fNum(c.d.frameMs, 1, c.hasData), "ms",
            c.d.fps > 0 ? fcol : C_VALUE_DIM);
    updCell(2, fFps(c.d.fpsLow, c.hasData), "",
            fpsColor(c.d.fpsLow, c.hasData, c.stale));
  }
}

// ===================== 第 4 页：IO / 网络 =====================
static void drawStaticIO() {
  clearBeforeCards();
  drawTitleBar(PAGE_NAME[P_IO], P_IO);
  drawBlePill();

  drawCardFrame(CPU_X, "DISK", "STORAGE", C_CYAN, C_CARD_ED);
  drawCardFrame(GPU_X, "NET", "NETWORK", C_OK, C_CARD_ED);

  drawRowLabel(CPU_X, 0, "READ");
  drawRowLabel(CPU_X, 1, "WRITE");
  drawRowLabel(CPU_X, 2, "USED");
  drawRowLabel(GPU_X, 0, "UP");
  drawRowLabel(GPU_X, 1, "TOTAL");
  drawRowLabel(GPU_X, 2, "LOSS");

  // 中块：链路与系统诊断（两行成对数值）
  drawMidFrame(PK_Y, PK_H, C_CARD_ED);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_LABEL_DIM);
  tft.drawString("LINK / SYSTEM", PK_LBL_X, PK_HEAD_Y);
  tft.setTextColor(C_LABEL);
  tft.drawString("RATE", PK_LBL_X, PK_ROW_Y(0) + 4);
  tft.drawString("UP",   PK_LBL_X, PK_ROW_Y(1) + 4);
  // 右列的 "PKTS"/"PING" 标签由 updPairRow 在自己的区域里画（见那边注释）

  static const char *labels[3] = {"MEM", "PROC", "FPS"};
  drawStripFrame(labels);
}

#if FORCE_PAGE
// 调试专用（FORCE_PAGE 打开时才编译）：中块标题右侧显示"固件刻度 + 最近一包 raw"。
//   S100     = 固件用的是 x100 刻度表 —— 这就是"新版固件"的标记（旧版会显示 S10）
//   R15/1    = 最近一包 net_down/net_up 的原始整数，和 tools/watch_net.py 打印的
//              raw 一栏对上，就说明两端刻度一致；若屏幕数值恰好是 raw 的 1/10 或 10 倍，
//              就是两端刻度表不一致（差 10 倍），不是采集错了。
static void updProtoDebug() {
  char b[32];
  snprintf(b, sizeof(b), "S%u R%u/%u", (unsigned)protoFieldScale(24),
           (unsigned)protoLastRaw(24), (unsigned)protoLastRaw(25));
  const int rx = PK_LBL_X + 96;              // "LINK / SYSTEM" 占 78px，右边是空的
  const int w  = PK_GPU_R - rx;
  Canvas c = regionBegin(rx, PK_HEAD_Y - 3, w, 13, C_CARD);
  TFT_eSPI &g = *c.g;
  g.setTextFont(1);
  g.setTextSize(1);
  g.setTextDatum(TR_DATUM);
  g.setTextColor(C_LABEL_DIM);
  g.drawString(b, c.dx + PK_GPU_R, c.dy + PK_HEAD_Y);
  regionEnd(c, rx, PK_HEAD_Y - 3, w, 13);
}
#endif

static void pageIO(const FrameCtx &c) {
  // ---- DISK 卡片：吞吐做主角 ----
  float total = (c.d.diskRead > 0 ? c.d.diskRead : 0) + (c.d.diskWrite > 0 ? c.d.diskWrite : 0);
  if (c.full || lastData.diskRead != c.d.diskRead || lastData.diskWrite != c.d.diskWrite) {
    updHero(CPU_X, "R+W", fMbs(total, c.hasData), mbsUnit(total, c.hasData),
            c.stale ? C_VALUE_DIM : C_CYAN,
            total * 100.0f / DISK_BAR_MAX, c.stale ? C_VALUE_DIM : C_CYAN);
  }
  if (c.full || lastData.diskRead != c.d.diskRead) {
    updRow(CPU_X, 0, fMbs(c.d.diskRead, c.hasData), mbsUnit(c.d.diskRead, c.hasData),
           c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.diskWrite != c.d.diskWrite) {
    updRow(CPU_X, 1, fMbs(c.d.diskWrite, c.hasData), mbsUnit(c.d.diskWrite, c.hasData),
           c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.diskUsed != c.d.diskUsed) {
    updRow(CPU_X, 2, fZero(c.d.diskUsed, 1, c.hasData), "%",
           c.stale ? C_VALUE_DIM : stateColor(c.d.diskUsed, C_OK));
  }

  // ---- NET 卡片：下行做主角（单位 MB/s / KB/s，和磁盘保持一致）----
  float ntot = (c.d.netDown > 0 ? c.d.netDown : 0) + (c.d.netUp > 0 ? c.d.netUp : 0);
  if (c.full || lastData.netDown != c.d.netDown) {
    updHero(GPU_X, "DOWN", fMbs(c.d.netDown, c.hasData), mbsUnit(c.d.netDown, c.hasData),
            c.stale ? C_VALUE_DIM : C_OK,
            c.d.netDown * 100.0f / NET_BAR_MAX, c.stale ? C_VALUE_DIM : C_OK);
  }
  if (c.full || lastData.netUp != c.d.netUp) {
    updRow(GPU_X, 0, fMbs(c.d.netUp, c.hasData), mbsUnit(c.d.netUp, c.hasData),
           c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.netDown != c.d.netDown || lastData.netUp != c.d.netUp) {
    updRow(GPU_X, 1, fMbs(ntot, c.hasData), mbsUnit(ntot, c.hasData),
           c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.pingLoss != c.d.pingLoss) {
    updRow(GPU_X, 2, fZero(c.d.pingLoss, 0, c.hasData), "%",
           c.stale ? C_VALUE_DIM : (c.d.pingLoss > 0 ? C_WARN : C_OK));
  }

  // ---- 中块：采样率/包数 + 运行时长/延迟 ----
  if (c.full || (c.now / 1000) != lastTick1s) {
    updPairRow(PK_ROW_Y(0), PK_VAL_X, PK_CPU_R, PK_GPU_R,
               c.stale ? C_VALUE_DIM : C_LABEL, c.stale ? C_VALUE_DIM : C_LABEL,
               fRate(), fPkts(blePacketCount()), "", "PKTS");
    String up = c.hasData && c.d.uptimeMin > 0
                  ? (c.d.uptimeMin >= 1440 ? String(c.d.uptimeMin / 1440, 0) + "d"
                                           : String(c.d.uptimeMin / 60, 0) + "h")
                  : "--";
    String ping = c.d.pingMs > 0 ? String(c.d.pingMs, 0) : "--";
    updPairRow(PK_ROW_Y(1), PK_VAL_X, PK_CPU_R, PK_GPU_R,
               c.stale ? C_VALUE_DIM : C_LABEL,
               c.stale ? C_VALUE_DIM : (c.d.pingMs > 100 ? C_WARN : C_LABEL),
               up, ping, "", "PING");
#if FORCE_PAGE
    updProtoDebug();          // 调试：右侧显示固件刻度 + 最近一包 raw
#endif
  }

  // ---- 底栏：内存 / 进程数 / 帧率 ----
  if (c.full || lastData.memUsed != c.d.memUsed) {
    updCell(0, fPct(c.d.memUsed, c.hasData), "%",
            c.stale ? C_VALUE_DIM : (c.aMem ? C_ALERT : stateColor(c.d.memUsed, C_OK)));
  }
  if (c.full || lastData.procCount != c.d.procCount) {
    updCell(1, fZero(c.d.procCount, 0, c.hasData), "", c.stale ? C_VALUE_DIM : C_LABEL);
  }
  if (c.full || lastData.fps != c.d.fps) {
    updCell(2, fFps(c.d.fps, c.hasData), "", fpsColor(c.d.fps, c.hasData, c.stale));
  }
}

// ===================== 主刷新 =====================
static void drawPageStatic(uint8_t page) {
  switch (page) {
    case P_THERMAL: drawStaticThermal(); break;
    case P_TREND:   drawStaticTrend();   break;
    case P_IO:      drawStaticIO();      break;
    default:        drawStaticOverview(); break;
  }
}

void drawStaticUI() {
  drawStaticOverview();
}

// ===================== 开机画面：状态与前置声明 =====================
//  实现在文件后半的「开机动画」一节。主循环的入口 drawMainUI() 定义在它前面，
//  而 C++ 里"用在前、定义在后"必须先有声明，所以阶段枚举、共享状态和要用到的
//  几个函数都在这里先声明一次。
enum UiPhase : uint8_t { UI_BOOT = 0, UI_DATA = 1 };

static uint8_t  uiPhase = UI_BOOT;
static uint32_t bootStartMs = 0;
static bool     bootLastPill = false;
static char     bootLastHint[32] = {0};
static bool     bootHintDrawn = false;

static void bootDrawStatic();
static void bootClearResidue();
static bool bootUpdate();

void drawMainUI() {
  // 还没连上 PC 就停在启动画面：这时候数据页面没有任何有意义的内容可画
  // （满屏 "--" + NO SIGNAL），不如用来交代"在等什么"。
  if (uiPhase == UI_BOOT) {
    if (!bootUpdate()) return;
    uiPhase = UI_DATA;
    bootClearResidue();
    firstRun = true;              // 让数据页整体重画一次（标题栏/卡片/底栏）
  }

  FrameCtx c;
  c.hasData = bleDataSnapshot(c.d);
  c.now = millis();
  c.stale = (!c.hasData) || ((uint32_t)(c.now - bleLastPacketMs()) > DATA_TIMEOUT_MS);

  c.aCpuT = alarmUpdate(alarmCpuTemp, c.d.cpuTemp,  ALARM_CPU_TEMP);
  c.aGpuT = alarmUpdate(alarmGpuTemp, c.d.gpuTemp,  ALARM_GPU_TEMP);
  c.aHot  = alarmUpdate(alarmGpuHot,  c.d.gpuHotspot, ALARM_GPU_HOT);
  c.aVram = alarmUpdate(alarmVramTemp, c.d.gpuMemTemp, ALARM_VRAM_TEMP);
  c.aHddT = alarmUpdate(alarmHddTemp, c.d.hddTemp,  ALARM_HDD_TEMP);
  c.aCpuL = alarmUpdate(alarmCpuLoad, c.d.cpuLoad,  ALARM_CPU_LOAD);
  c.aGpuL = alarmUpdate(alarmGpuLoad, c.d.gpuLoad,  ALARM_GPU_LOAD);
  c.aMem  = alarmUpdate(alarmMem,     c.d.memUsed,  ALARM_MEM);

  c.blinkOn = ((c.now / BLINK_MS) & 1) == 0;
  static bool lastBlinkOn = true;
  c.blinkFlip = (c.blinkOn != lastBlinkOn);
  lastBlinkOn = c.blinkOn;

  // 过期置灰
  c.cPwr  = c.stale ? C_VALUE_DIM : C_CYAN;
  c.cClk  = c.stale ? C_VALUE_DIM : C_LABEL;
  c.cVram = c.stale ? C_VALUE_DIM : C_GPU;

  // 自开机极值（只在拿到有效数据时更新）
  if (c.hasData && !c.stale) {
    if (c.d.cpuLoad > maxCpuLoad) maxCpuLoad = c.d.cpuLoad;
    if (c.d.gpuLoad > maxGpuLoad) maxGpuLoad = c.d.gpuLoad;
    if (c.d.cpuTemp > maxCpuTemp) maxCpuTemp = c.d.cpuTemp;
    if (c.d.gpuTemp > maxGpuTemp) maxGpuTemp = c.d.gpuTemp;
  }

  // 采样间隔统计（波形每推进一个点算一次；主循环 200ms 会合并更快的包）
  if (waveIdx != waveIdxForSample) {
    if (lastPushMs != 0) {
      uint32_t dt = c.now - lastPushMs;
      if (dt > 0 && dt < 60000UL) sampleMsEma = sampleMsEma ? (sampleMsEma * 3 + dt) / 4 : dt;
    }
    lastPushMs = c.now;
    waveIdxForSample = waveIdx;
  }

  // 翻页：有告警就跳到"能看到该指标"的那一页提醒一下，最多钉 ALARM_PIN_MS。
  // 只钉"告警刚出现/换类"那一次，超时后即使还响着也恢复轮播——否则像 NVMe 温度、
  // HX 版 CPU 温度这种长期超阈值的指标会把画面永久占住，轮播直接失效。
  bool alarmT = c.aCpuT || c.aGpuT || c.aHddT || c.aHot || c.aVram;
  bool alarmL = c.aCpuL || c.aGpuL || c.aMem;
  bool anyAlarm = !c.stale && (alarmT || alarmL);
  uint8_t want = alarmT ? P_THERMAL : P_OVERVIEW;

  static bool lastAlarm = false;
  static uint8_t lastWant = P_OVERVIEW;
  static uint32_t pinUntilMs = 0;

  if (anyAlarm && (!lastAlarm || want != lastWant)) pinUntilMs = c.now + ALARM_PIN_MS;
  bool pinned = anyAlarm && (int32_t)(pinUntilMs - c.now) > 0;
  lastAlarm = anyAlarm;
  lastWant  = want;

  bool pageSwitched = false;
#if FORCE_PAGE
  // 调试：恒定停在这一页，不轮播也不被告警跳走（告警配色/边框呼吸照旧）。
  // 注意 FORCE_PAGE 是"屏幕上的第几页"（1 基，跟标题栏页码圆点一致），
  // 而内部 curPage 是 0 基枚举，所以要减 1——直接写 4 会落到 drawPageStatic 的
  // default 分支画成概览页（这个坑踩过一次）。
  static_assert(FORCE_PAGE >= 1 && FORCE_PAGE <= PAGE_COUNT, "FORCE_PAGE 越界（1..4，0=关闭）");
  (void)pinned;
  uint8_t forceIdx = (uint8_t)(FORCE_PAGE - 1);
  if (curPage != forceIdx) { curPage = forceIdx; pageSwitched = true; }
  pageStartMs = c.now;
#else
  if (firstRun) {
    pageStartMs = c.now;
  } else if (pinned) {
    if (curPage != want) { curPage = want; pageSwitched = true; }
    // 不清零 pageStartMs：锁页一结束就接着往下翻，不再白等一个 PAGE_MS
  } else if ((uint32_t)(c.now - pageStartMs) >= PAGE_MS) {
    curPage = (uint8_t)((curPage + 1) % PAGE_COUNT);
    pageStartMs = c.now;
    pageSwitched = true;
  }
#endif

  // 行轮播（把电压/热点/显存温度/GPU频率/最忙核心/机箱风扇塞进固定位置）
  if ((uint32_t)(c.now - rotStartMs) >= ROT_MS) {
    rotStartMs = c.now;
    rotIdx = (uint8_t)((rotIdx + 1) % 3);
  }
  c.rot = rotIdx;
  c.rotChanged = (rotIdx != lastRotIdx);
  lastRotIdx = rotIdx;

  c.full = firstRun || pageSwitched || (c.stale != lastStale);

  if (c.full) {
    drawPageStatic(curPage);      // 不清屏：实心卡片互相覆盖，避免整屏闪一下
  } else if (lastBle != bleConnected) {
    drawTitleBar(PAGE_NAME[curPage], curPage);
    drawBlePill();
  }
  lastBle = bleConnected;

  // 型号名到达/变化时刷新卡片副标题
  char nc[PROTO_NAME_MAX], ng[PROTO_NAME_MAX];
  bleDeviceNames(nc, sizeof(nc), ng, sizeof(ng));
  bool namesChanged = (strcmp(nc, lastNameCpu) != 0) || (strcmp(ng, lastNameGpu) != 0);
  if (namesChanged) {
    strncpy(lastNameCpu, nc, sizeof(lastNameCpu) - 1);
    strncpy(lastNameGpu, ng, sizeof(lastNameGpu) - 1);
  }
  if (curPage != P_TREND) {          // 趋势页没有卡片
    if (namesChanged || c.full) {
      // IO 页两张卡片是 DISK / NETWORK，副标题固定，**不能跟着 CPU/GPU 型号名走**：
      // 以前只特判了左卡片，右卡片仍取 ng，于是网络卡片下面写着 "NVIDIA GeForce ..."
      if (curPage == P_IO) {
        updSubtitle(CPU_X, "STORAGE");
        updSubtitle(GPU_X, "NETWORK");
      } else {
        updSubtitle(CPU_X, nc[0] ? nc : "PROCESSOR");
        updSubtitle(GPU_X, ng[0] ? ng : "GRAPHICS");
      }
    }
  }

  switch (curPage) {
    case P_THERMAL: pageThermal(c); break;
    case P_TREND:   pageTrend(c);   break;
    case P_IO:      pageIO(c);      break;
    default:        pageOverview(c); break;
  }

  // 告警：数值常红（在页面里处理）+ 卡片边框呼吸（这里只画边框，不重画数字）
  // 趋势页没有卡片，跳过，别把边框画到大图上
  if (curPage != P_TREND && (c.full || c.blinkFlip)) {
    if (c.stale) {
      drawCardBorder(CPU_X, C_CPU_DIM);
      drawCardBorder(GPU_X, C_GPU_DIM);
    } else {
      drawCardBorder(CPU_X, (c.aCpuT || c.aCpuL) ? (c.blinkOn ? C_ALERT : C_CPU_DIM) : C_CPU_DIM);
      drawCardBorder(GPU_X, (c.aGpuT || c.aGpuL) ? (c.blinkOn ? C_ALERT : C_GPU_DIM) : C_GPU_DIM);
    }
  }

  firstRun = false;
  lastStale = c.stale;
  lastTick10s = c.now / 10000;
  lastTick1s  = c.now / 1000;
  lastData = c.d;
}

// ===================== 开机动画 =====================
//  为什么要有它：上电到 PC 端连上之间要等几秒（PC 端程序没启动时会一直等），
//  这段时间如果直接显示数据页面，就是满屏 "--" 加 NO SIGNAL —— 看着像坏了。
//  启动画面把这段时间用来交代"在等什么"：BLE 在广播 / 等 PC 连接 / 已连接等数据，
//  另外把 BLE 设备名印出来，方便在 PC 端程序里对上号。
//
//  动效只有"一条来回扫的指示条 + 状态文字 + 三点省略号"，全部走离屏画布
//  （regionBegin/regionEnd），不留"先擦后画"的空窗 —— 与数据页的刷新规矩一致。
//  扫描条一个来回 2.4 秒，帧率由 config.h 的 BOOT_FRAME_MS 决定（默认 33ms≈30fps）。
//
//  版面（320x240，全部用 TL_DATUM + 手工居中：仿真里只实现了 TL/TR 基准）
//    y 0..21    标题栏（复用 drawTitleBar："PC MONITOR" + BOOTING + 链路胶囊）
//    y 62..88   Font4 大标题 "PC MONITOR"
//    y 98..106  小字 "SYSTEM TELEMETRY"
//    y 114..120 三色强调条（CPU 绿 / 青 / GPU 橙）
//    y 146..156 扫描条（底槽 + 44px 高亮来回扫）
//    y 162..178 状态文字（字体 1 放大 2 倍）
//    y 188..196 超过 BOOT_HINT_MS 还没连上时追加的一行提示
//    y 212..220 BLE 设备名
#define BOOT_TITLE_Y   62
#define BOOT_CAP_Y     98
#define BOOT_ACC_Y     114
#define BOOT_ACC_W     120
#define BOOT_TRACK_X   60
#define BOOT_TRACK_Y   146
#define BOOT_TRACK_W   200
#define BOOT_TRACK_H   10
#define BOOT_SWEEP_W   44
#define BOOT_STATUS_Y  162
#define BOOT_HINT_Y    188
#define BOOT_DEV_Y     212
#define BOOT_SWEEP_MS  2400      // 扫描条一个来回

uint16_t uiFrameMs() {
  // 启动画面要 30fps 才像动画；数据页 5Hz 就够（而且没必要一直推大块 SPI）
  return (uiPhase == UI_BOOT) ? BOOT_FRAME_MS : (uint16_t)REFRESH_DELAY;
}

bool uiBooting() { return uiPhase == UI_BOOT; }

// 居中的小工具（TL_DATUM + 手工算宽度；字体 1 是 6px/字符，放大 n 倍就是 6n）
static int bootTextX(const char *s, uint8_t font, uint8_t size) {
  int w = (font == 1) ? (int)strlen(s) * 6 * (int)size
                      : (int)tft.textWidth(s, font);
  return (SCR_W - w) / 2;
}

static void bootDrawStatic() {
  drawTitleBar("BOOTING", -1);      // -1：四个页码点全灭（还没进任何一页）
  drawBlePill();
  bootLastPill = bleConnected;

  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_VALUE, C_BG);
  tft.drawString("PC MONITOR", bootTextX("PC MONITOR", 4, 1), BOOT_TITLE_Y);

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(C_LABEL_DIM, C_BG);
  tft.drawString("SYSTEM TELEMETRY", bootTextX("SYSTEM TELEMETRY", 1, 1), BOOT_CAP_Y);

  // 三色强调条：左右各 36px 品牌色，中间青色，段间留 6px
  {
    int x = (SCR_W - BOOT_ACC_W) / 2;
    const int seg = 36, gap = 6;
    tft.fillRect(x, BOOT_ACC_Y, seg, 6, C_CPU);
    tft.fillRect(x + seg + gap, BOOT_ACC_Y, seg, 6, C_CYAN);
    tft.fillRect(x + 2 * (seg + gap), BOOT_ACC_Y, seg, 6, C_GPU);
  }

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(C_LABEL_DIM, C_BG);
  tft.drawString(BLE_DEVICE_NAME, bootTextX(BLE_DEVICE_NAME, 1, 1), BOOT_DEV_Y);
}

// 状态文字：0 刚上电 / 1 等 PC 连接 / 2 已连接等数据 / 3 收到数据（马上切页）
static const char *bootStatusText(uint8_t st, uint8_t dots) {
  static char buf[32];
  switch (st) {
    case 0:  return "BLE STARTING";
    case 1:  snprintf(buf, sizeof(buf), "WAITING FOR PC%.*s", (int)dots, "..."); return buf;
    case 2:  return "LINKED - WAITING DATA";
    default: return "RECEIVING DATA";
  }
}

static uint16_t bootStatusColor(uint8_t st) {
  switch (st) {
    case 0: case 1: return C_WARN;    // 还在等：黄
    case 2:         return C_CYAN;    // 连上了但没数据：青
    default:        return C_OK;      // 数据到了：绿
  }
}

// 有没有"真数据"？**握手包不算**。
//  PC 端连上后会先发一个全 0xFFFF 的握手包（proto_v2.pack_test()，用来确认协议通）。
//  如果只看"收到过包"就切页，屏幕会先切到数据页显示一片 "--"，一秒后才出数 ——
//  那正是启动画面想避免的画面。所以这里要求至少有一个字段不是哨兵(-1)。
//  （HardwareData 全是 float，按 float 扫一遍即可；ble_service.cpp 的 markAllInvalid
//    用的是同一个前提，那边有 static_assert 兜着。）
static bool bootHasRealData() {
  HardwareData d;
  if (!bleDataSnapshot(d)) return false;
  const float *p = reinterpret_cast<const float *>(&d);
  for (size_t i = 0; i < PROTO_FIELD_COUNT; i++) {
    if (p[i] >= 0.0f) return true;
  }
  return false;
}

static void bootDrawDynamic(uint32_t now) {
  uint8_t st;
  if (bleConnected && bootHasRealData())      st = 3;
  else if (bleConnected)                      st = 2;
  else if ((uint32_t)(now - bootStartMs) < 700) st = 0;
  else                                        st = 1;

  // ---- 扫描条：底槽 + 44px 高亮来回扫 ----
  {
    Canvas c = regionBegin(BOOT_TRACK_X, BOOT_TRACK_Y, BOOT_TRACK_W, BOOT_TRACK_H, C_BG);
    TFT_eSPI &g = *c.g;
    g.fillRect(BOOT_TRACK_X + c.dx, BOOT_TRACK_Y + c.dy, BOOT_TRACK_W, BOOT_TRACK_H, C_BAR_BG);
    g.drawRect(BOOT_TRACK_X + c.dx, BOOT_TRACK_Y + c.dy, BOOT_TRACK_W, BOOT_TRACK_H, C_CARD_ED);

    int span = BOOT_TRACK_W - BOOT_SWEEP_W;
    uint32_t ph = ((uint32_t)(now - bootStartMs) % BOOT_SWEEP_MS) * span / (BOOT_SWEEP_MS / 2);
    int pos = (ph > (uint32_t)span) ? (int)(2 * (uint32_t)span - ph) : (int)ph;
    g.fillRect(BOOT_TRACK_X + pos + c.dx, BOOT_TRACK_Y + 3 + c.dy,
               BOOT_SWEEP_W, BOOT_TRACK_H - 6, C_CYAN);
    regionEnd(c, BOOT_TRACK_X, BOOT_TRACK_Y, BOOT_TRACK_W, BOOT_TRACK_H);
  }

  // ---- 状态文字（每帧重推一次：走画布，所以不会"先擦后画"闪一下）----
  {
    uint8_t dots = (uint8_t)(((uint32_t)(now - bootStartMs) / 400) % 4);
    const char *txt = bootStatusText(st, dots);
    int w = (int)strlen(txt) * 6 * 2;
    int x = (SCR_W - w) / 2;
    Canvas c = regionBegin(x, BOOT_STATUS_Y, w, 16, C_BG);
    TFT_eSPI &g = *c.g;
    g.setTextFont(1);
    g.setTextSize(2);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(bootStatusColor(st), C_BG);
    g.drawString(txt, x + c.dx, BOOT_STATUS_Y + c.dy);
    regionEnd(c, x, BOOT_STATUS_Y, w, 16);
  }

  // ---- 迟迟连不上时追加一行提示（只在文字变化时重画）----
  {
    const char *hint = (st <= 1 && (uint32_t)(now - bootStartMs) > BOOT_HINT_MS)
                       ? "START THE PC APP ON THIS PC" : "";
    if (strcmp(hint, bootLastHint) != 0) {
      strncpy(bootLastHint, hint, sizeof(bootLastHint) - 1);
      bootLastHint[sizeof(bootLastHint) - 1] = 0;
      int w = 29 * 6;                       // 按最长的那句留宽度，避免长短句来回擦不干净
      int x = (SCR_W - w) / 2;
      Canvas c = regionBegin(x, BOOT_HINT_Y, w, 10, C_BG);
      TFT_eSPI &g = *c.g;
      g.setTextFont(1);
      g.setTextSize(1);
      g.setTextDatum(TL_DATUM);
      g.setTextColor(C_LABEL_DIM, C_BG);
      if (hint[0]) g.drawString(hint, x + c.dx, BOOT_HINT_Y + c.dy);
      regionEnd(c, x, BOOT_HINT_Y, w, 10);
      bootHintDrawn = true;
    }
  }
}

// 启动画面 → 数据页面：补清数据页盖不到的缝隙。
//  启动画面是整屏构图的，而数据页只覆盖「标题栏 / 两张卡片 / 中块 / 底栏」四块，
//  卡片之间与四周那几条 4px 缝隙不会被覆盖 —— 不补清就会留下启动画面的残留。
//  （这是本项目切页时的同一条规矩：新页面盖不到的区域要自己补清。）
static void bootClearResidue() {
  const uint16_t bg = C_BG;
  tft.fillRect(0, TITLE_H, SCR_W, CARD_Y - TITLE_H, bg);                    // y 22..25
  tft.fillRect(0, CARD_Y + CARD_H, SCR_W, MID_Y - (CARD_Y + CARD_H), bg);   // y 138..141
  tft.fillRect(0, MID_Y + MID_H, SCR_W, BOT_Y - (MID_Y + MID_H), bg);       // y 204..207
  tft.fillRect(0, CARD_Y, PAD, MID_Y + MID_H - CARD_Y, bg);                 // 左 4px
  tft.fillRect(SCR_W - PAD, CARD_Y, PAD, MID_Y + MID_H - CARD_Y, bg);       // 右 4px
  tft.fillRect(CPU_X + CARD_W, CARD_Y, GAP, CARD_H, bg);                    // 两卡之间的 4px
}

// 返回 true = 这一帧起切到数据页面
static bool bootUpdate() {
  uint32_t now = millis();

  // 链路胶囊随连接状态翻转（它画在标题栏上，得整条重画才没有接缝）
  if (bleConnected != bootLastPill) {
    bootLastPill = bleConnected;
    drawTitleBar("BOOTING", -1);
    drawBlePill();
  }

  bootDrawDynamic(now);

  // 升级条件：**连上了 并且 收到了真数据**（不是那个全无效的握手包），
  // 而且启动画面至少显示了 BOOT_MIN_MS。只看 bleConnected 不够：连上到第一包
  // 真数据之间屏幕还是没东西可显示；最短时长是为了"PC 端秒连"时动画不会只闪半帧。
  return bleConnected && bootHasRealData()
         && (uint32_t)(now - bootStartMs) >= BOOT_MIN_MS;
}

// ===================== 初始化 =====================
void screenInit() {
  tft.init();
  tft.setRotation(1);
  // 打开 GLCD 字体的 CP437 修正，否则度符号(0xF8)会错位一格画成别的字形
  tft.setAttribute(CP437_SWITCH, 1);
  tft.fillScreen(C_BG);
  tft.setTextColor(C_VALUE, C_BG);
  tft.setTextDatum(TL_DATUM);

  // 背光引脚 TFT_BL 由 tft_setup.h 统一定义（12）。
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  memset(&lastData, 0, sizeof(HardwareData));
  uiPhase = UI_BOOT;
  bootStartMs = millis();
  bootLastPill = false;
  bootLastHint[0] = 0;
  bootHintDrawn = false;
  firstRun = true;
  lastStale = false;
  lastBle = false;
  curPage = P_OVERVIEW;
  pageStartMs = millis();
  lastTick10s = 0;
  lastTick1s  = 0;
  lastSeenWaveIdx = 0xFFFF;
  lastTrendStale = false;
  lastTrendPushMs = 0;
  waveIdxForSample = 0xFFFF;
  lastPushMs = 0;
  sampleMsEma = 0;
  maxCpuLoad = maxGpuLoad = 0.0f;
  maxCpuTemp = maxGpuTemp = 0.0f;
  shownMaxCpu = shownMaxGpu = -1.0f;
  shownMaxCpuT = shownMaxGpuT = -1.0f;
  rotIdx = 0;
  lastRotIdx = 0xFF;
  rotStartMs = millis();
  lastNameCpu[0] = 0;
  lastNameGpu[0] = 0;
  alarmCpuTemp = alarmGpuTemp = alarmGpuHot = alarmVramTemp = alarmHddTemp = false;
  alarmCpuLoad = alarmGpuLoad = alarmMem = false;

  // 离屏画布：286x112x2 ≈ 64KB，趋势图和所有局部刷新共用
  canvas.setColorDepth(16);
  canvas.createSprite(PLOT_W, TREND_PLOT_H);
  if (!canvas.created()) {
    Serial.println("[UI] canvas alloc failed -> direct draw fallback (mild flicker)");
  } else {
    Serial.printf("[UI] canvas %dx%d ok, free heap %u\n",
                  (int)PLOT_W, (int)TREND_PLOT_H, (unsigned)ESP.getFreeHeap());
  }

  bootDrawStatic();     // 开机先停在启动画面：连上 PC 并收到数据后才画数据页
}
