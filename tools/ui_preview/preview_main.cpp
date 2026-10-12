// ============================================================================
//  离线预览：喂假数据给真实主循环（PC_Monitor.ino 的 setup/loop），
//  逐页渲染 + 像素级断言（含"标签是否被数值压到"的逐像素比对）。
// ============================================================================
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "TFT_eSPI.h"
#include "config.h"
#include "screen_draw.h"      // uiBooting() / drawMainUI()：开机画面阶段判定要用
#include "proto.h"            // PROTO_FIELD_COUNT：造"全无效握手包"用

extern uint16_t SCREEN[320 * 240];
extern TFT_eSPI tft;
extern void simAdvanceMs(unsigned long ms);
extern void simSaveBmp(const char *path);
extern void simDumpRegion(const char *title, int x0, int y0, int x1, int y1, int scale);
extern int simCountColor(int x0, int y0, int x1, int y1, uint16_t color);
extern int simRightmostNonBg(int x0, int y0, int x1, int y1, uint16_t bg);
extern int simLeftmostNonBg(int x0, int y0, int x1, int y1, uint16_t bg);
extern void simBBox(const char *name, uint16_t color);
extern void simRunRow(const char *name, int y, uint16_t color);
extern void simRunCol(const char *name, int x, uint16_t color);
extern void simInkMap(const char *title, int x0, int y0, int x1, int y1);
extern void simInkMapExpected(const char *text, int tx, int ty, int x0, int y0, int x1, int y1);
extern int simLabelIntact(const char *text, int x, int y);
extern int simLabelIntactN(const char *text, int x, int y, int rows);
extern int simTextIntact(const char *text, int x, int y, int rows, uint16_t color);
extern uint16_t simCardBg(void);
extern int  simTextOnScreen(void);
extern int  simFillScreenCalls(void);
extern void simResetCounters(void);

void setup();      // 来自 PC_Monitor.ino
void loop();

// ---------------- 假的 BLE 数据源（替代 ble_service.cpp）----------------
static HardwareData g_data;
static uint32_t g_seq = 0;
static uint32_t g_lastMs = 0;
static uint32_t g_taken = 0;

bool bleDataSnapshot(HardwareData &out) { out = g_data; return g_seq != 0; }
bool bleDataTake(HardwareData &out, uint32_t &seq) {
  out = g_data; seq = g_seq;
  bool isNew = (seq != g_taken);
  g_taken = seq;
  return isNew;
}
uint32_t bleLastPacketMs(void) { return g_lastMs; }
uint32_t blePacketCount(void) { return g_seq; }
void bleDeviceNames(char *cpu, size_t cpuSz, char *gpu, size_t gpuSz) {
  snprintf(cpu, cpuSz, "i5-12400");
  snprintf(gpu, gpuSz, "RTX 4060");
}
void bleInit() {}

// 数据刻意不触发告警（温度缓慢上升但不过 90），保证翻页节奏可预期
static void publish(int f) {
  HardwareData d;
  memset(&d, 0, sizeof(d));
  bool gaming = !(f >= 120 && f < 140);            // 中间一段"桌面"状态：FPS = 0
  int g = f % 150;                                 // 温度用回绕，别越跑越热引出冗余告警

  d.cpuLoad  = 20.0f + 45.0f * (1.0f + sinf(f / 11.0f)) / 2.0f;
  d.cpuTemp  = 46.0f + 0.14f * g;                  // 46..67 度
  d.cpuFan   = 1180.0f + 6.0f * g;
  d.cpuPower = 30.0f + d.cpuLoad * 0.9f;
  d.cpuClk   = 4200.0f;

  d.gpuLoad  = 30.0f + 32.0f * (1.0f + sinf(f / 19.0f)) / 2.0f;
  d.gpuTemp  = 58.0f + 0.06f * g;                  // 58..67 度
  d.gpuMem   = 2.0f + 10.0f * d.gpuLoad / 100.0f;  // 最高约 10.2 / 24 GB
  d.gpuFan   = 1500.0f;
  d.gpuPower = 55.0f + d.gpuLoad * 1.6f;

  d.fps      = gaming ? (72.0f + 70.0f * (1.0f + sinf(f / 7.0f)) / 2.0f) : 0.0f;
  d.frameMs  = d.fps > 0 ? 1000.0f / d.fps : -1.0f;
  // 1% low（协议第 31 个字段）：由 PC 端在**逐帧**数据上算好送来，
  // 固件只负责显示。这里造一个明显低于平均值的低帧率（真实场景里
  // 实测出现过平均 57fps / 1% low 12fps），用来验证 LOW 那一格确实变了。
  // 注意"桌面"状态下它和 fps 一样是无效（-1）—— 没在跑游戏就没有 1% low。
  d.fpsLow   = gaming ? (d.fps * 0.42f) : -1.0f;
  d.memUsed  = 52.0f + 22.0f * (1.0f + sinf(f / 29.0f)) / 2.0f;
  d.hddTemp  = 38.5f;
  d.gpuMemTotal = 24.0f;

  // 新增的一批（第 4 页与轮播行用）
  d.cpuVolt    = 1.24f + 0.03f * sinf(f / 9.0f);
  d.cpuCoreMax = 55.0f + 30.0f * (1.0f + sinf(f / 5.0f)) / 2.0f;
  d.gpuHotspot = d.gpuTemp + 12.0f;
  d.gpuMemTemp = d.gpuTemp + 6.0f;
  d.gpuClk     = 2400.0f + 120.0f * sinf(f / 13.0f);
  d.gpuMemCtrl = 20.0f + 20.0f * (1.0f + sinf(f / 17.0f)) / 2.0f;
  d.moboFan    = 900.0f + 40.0f * sinf(f / 21.0f);
  d.diskRead   = 12.0f + 10.0f * (1.0f + sinf(f / 3.0f)) / 2.0f;
  d.diskWrite  = 4.0f + 6.0f * (1.0f + sinf(f / 5.0f)) / 2.0f;
  d.diskUsed   = 71.2f;
  d.netDown    = 2.0f + 8.0f * (1.0f + sinf(f / 7.0f)) / 2.0f;
  d.netUp      = 0.4f + 1.2f * (1.0f + sinf(f / 11.0f)) / 2.0f;
  d.pingMs     = 18.0f + 6.0f * sinf(f / 23.0f);
  d.pingLoss   = 0.0f;
  d.procCount  = 280.0f + 12.0f * sinf(f / 31.0f);
  d.uptimeMin  = 5120.0f + f;

  g_data = d;
  g_seq++;
  g_lastMs = millis();
}

// 全字段都是"读不到"（-1）的包 —— 就是 PC 端连上后立刻发的那个握手包
// （pc_side/proto_v2.py 的 pack_test()）。它只用来确认协议通，没有任何读数。
static void publishHandshake(void) {
  HardwareData d;
  float *p = (float *)&d;
  for (size_t i = 0; i < PROTO_FIELD_COUNT; i++) p[i] = -1.0f;
  g_data = d;
  g_seq++;
  g_lastMs = millis();
}

// ---------------- 页面识别（靠每页独有的像素特征，和渲染代码解耦）----------------
//  趋势页：大图底色 0x0861
//  IO 页：左卡片主指标条用青色 0x07FF（概览页的主指标条是绿/橙）
//  散热页：中块三条温度对比的底槽 0x0841
//  概览页：以上都不是
//  （44/166/142/62 是 screen_draw.cpp 里 MET_BAR_X/MET_BAR_W/MID_Y/MID_H 的值）
static int detectPage() {
  if (uiBooting()) return 0;         // 0 = 还在开机画面（不是任何一页）
  if (simCountColor(26, 42, 312, 150, 0x0861) > 200) return 3;
  // IO 页的左卡片主指标是青色的（数字 + 条），概览/散热的主指标是绿/橙
  if (simCountColor(12, 48, 150, 81, 0x07FF) > 80) return 4;
  if (simCountColor(44, 142, 44 + 166, 142 + 62, 0x0841) > 500) return 2;
  return 1;
}

static int pageDotOk(int page) {
  int x = 162 + (page - 1) * 16;                 // DOT_X + i*DOT_DX
  return simCountColor(x - 3, 8, x + 4, 15, 0xFFFF);   // 当前页是实心白点
}

// 标签列逐像素比对（见 preview_impl.cpp: simLabelIntact）
static void checkLabels(const char *const *names, const int (*pos)[2], int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int b = simLabelIntact(names[i], pos[i][0], pos[i][1]);
    if (b) { printf("    !! 标签 %-8s 被覆盖 %d 像素\n", names[i], b); bad += b; }
  }
  printf("    标签逐像素校验：%s（%d 个标签）\n", bad ? "失败" : "全部完好", n);
}

// 字体 4 的像素宽度（桩里用 Font32 的真实宽度表），
// 用来反推"主指标大字"的墨迹范围：数值算错/单位算错，宽度就对不上。
// 注意桩里 textWidth(s, n) 的第二个参数是【字体号】，不是字号倍数 —— 传 1 会按
// 字体 1 的 6px/字符 算，宽度全错（踩过一次）。
static int w4(const char *s) {
  static TFT_eSPI g;
  return g.textWidth(String(s), 4);
}

int main() {
  tft._bind(SCREEN, 320, 240);
  setup();

  // ==================== 0) 开机画面（连上 PC 之前）====================
  //  上电后到"连上 PC 并收到第一包数据"之间，屏幕上应该是启动画面，而不是数据页
  //  （后者在没数据时是满屏 "--" + NO SIGNAL，看着像坏了）。这一段按真实节奏
  //  （BOOT_FRAME_MS=33ms）逐帧推，验证：
  //    · 阶段：uiBooting() 为真，detectPage() 认不出任何一页（返回 0）
  //    · 构图：Font4 大标题的白墨左右居中
  //    · 动效：扫描条高亮逐帧在动，且始终在底槽范围内
  //    · 状态：等 PC = 黄，已连接等数据 = 青；超时后出现提示行
  //    · 频闪红线：启动动画的稳态帧"直接画到屏幕的文字次数"必须是 0
  //    · 切页：连上并收到数据后进数据页，且数据页盖不到的缝隙里没有启动画面残留
  //  版面数字（54/146/162/188/212）来自 screen_draw.cpp 的 BOOT_* 宏，改那边要同步改这里。
  printf("\n=== 0) 开机画面（连上 PC 之前）===\n");
  bleConnected = false;                       // 还没连上
  int bootBad = 0;
  const int bTitleY = 54, bTitleH = 32;       // 品牌位图（奶娃科技）32px 高
  const int bTrackX = 60, bTrackY = 146, bTrackW = 200;
  const int bStatusY = 162, bHintY = 188, bDevY = 212;

  printf("  上电即在启动画面=%d（期望 1）  detectPage()=%d（期望 0：不是任何一页）\n",
         (int)uiBooting(), detectPage());
  if (!uiBooting() || detectPage() != 0) { bootBad++; printf("  !! 上电没有停在启动画面\n"); }

  // ---- 品牌位图：白墨存在且左右居中（内置字体没有汉字，只能用位图；这里只验落点）----
  int tL = simLeftmostNonBg(0, bTitleY, 320, bTitleY + bTitleH, 0x0000);
  int tR = simRightmostNonBg(0, bTitleY, 320, bTitleY + bTitleH, 0x0000);
  int tWhite = simCountColor(0, bTitleY, 320, bTitleY + bTitleH, 0xFFFF);
  printf("  品牌位图：白墨 %d 像素，落点 x %d..%d（宽 %d，居中偏差 %d，期望 <=2）\n",
         tWhite, tL, tR, tR - tL + 1, abs(tL - (319 - tR)));
  if (tWhite < 500 || tL < 0 || tR < 0 || abs(tL - (319 - tR)) > 2) {
    bootBad++; printf("  !! 品牌位图缺失或没有居中\n");
  }

  // ---- 10 帧动画：高亮必须动，而且不越出底槽 ----
  int sweep[10];
  int steadyText = -1;
  for (int i = 0; i < 10; i++) {
    simAdvanceMs(BOOT_FRAME_MS);
    loop();
    // 只扫底槽内部（去掉 1px 边框），最左边的非底槽色像素就是高亮的左边界
    sweep[i] = simLeftmostNonBg(bTrackX + 1, bTrackY + 2, bTrackX + bTrackW - 1,
                                bTrackY + 8, 0x0841);
    if (i == 4) {
      // 稳态帧：动画在动，但不该有任何文字"直接画到屏幕"（都要走离屏画布，
      // 否则就是"先擦后画"的微闪来源 —— 和数据页的红线是同一条）
      simResetCounters();
      simAdvanceMs(BOOT_FRAME_MS);
      loop();
      steadyText = simTextOnScreen();
    }
  }
  int swMin = sweep[0], swMax = sweep[0], swDiff = 0;
  for (int i = 0; i < 10; i++) {
    if (sweep[i] < swMin) swMin = sweep[i];
    if (sweep[i] > swMax) swMax = sweep[i];
    if (i && sweep[i] != sweep[0]) swDiff++;
  }
  printf("  高亮左边界（10 帧）: %d %d %d %d %d %d %d %d %d %d\n",
         sweep[0], sweep[1], sweep[2], sweep[3], sweep[4], sweep[5], sweep[6], sweep[7],
         sweep[8], sweep[9]);
  printf("  在动（不同位置数 %d，期望 >=6）  范围 %d..%d（底槽内 %d..%d）\n",
         swDiff, swMin, swMax, bTrackX + 1, bTrackX + 1 + (bTrackW - 44 - 1));
  if (swDiff < 6 || swMin < bTrackX + 1 || swMax > bTrackX + bTrackW - 44) {
    bootBad++; printf("  !! 扫描条没动或跑出底槽\n");
  }
  printf("  扫描条青色像素=%d（期望 %d：44x4）\n",
         simCountColor(bTrackX, bTrackY, bTrackX + bTrackW, bTrackY + 10, 0x07FF), 44 * 4);
  printf("  稳态帧直接画到屏幕的文字次数=%d（期望 0）\n", steadyText);
  if (steadyText != 0) { bootBad++; printf("  !! 启动动画有文字直接画到屏幕（会微闪）\n"); }

  // ---- 状态行：等 PC 时是黄的（C_WARN）----
  int warnPix = simCountColor(20, bStatusY, 300, bStatusY + 16, 0xFFE0);
  printf("  状态行黄色像素=%d（期望 >0：还在等 PC）\n", warnPix);
  if (warnPix <= 0) { bootBad++; printf("  !! 等待状态没有用黄色标出\n"); }
  simSaveBmp("boot_1_waiting.bmp");

  // ---- 超过 BOOT_HINT_MS 还没连上：应出现提示行，且仍在启动画面 ----
  simAdvanceMs(BOOT_HINT_MS + 1000);
  loop();
  int hintPix = simCountColor(20, bHintY, 300, bHintY + 10, 0xD69A);
  printf("  等待 %lus 后：提示行暗灰像素=%d（期望 >0：START THE PC APP ON THIS PC）\n",
         (unsigned long)(BOOT_HINT_MS / 1000), hintPix);
  printf("  仍在启动画面=%d（期望 1：连不上就一直等，不切 -- 页面）\n", (int)uiBooting());
  if (hintPix <= 0 || !uiBooting()) { bootBad++; printf("  !! 超时提示没有出现或提前切页\n"); }
  int devPix = simCountColor(0, bDevY, 320, bDevY + 8, 0xD69A);
  printf("  底部 BLE 设备名暗灰像素=%d（期望 >0：%s）\n", devPix, BLE_DEVICE_NAME);
  if (devPix <= 0) { bootBad++; printf("  !! 没印出 BLE 设备名\n"); }
  simSaveBmp("boot_2_hint.bmp");

  // ---- 连上了但还没数据：状态转青（C_CYAN），仍然停在启动画面 ----
  bleConnected = true;
  simAdvanceMs(BOOT_FRAME_MS);
  loop();
  int cyanPix = simCountColor(20, bStatusY, 300, bStatusY + 16, 0x07FF);
  printf("  连上但无数据：状态行青色像素=%d（期望 >0）  仍在启动画面=%d（期望 1）\n",
         cyanPix, (int)uiBooting());
  if (cyanPix <= 0 || !uiBooting()) { bootBad++; printf("  !! 已连接状态没有转青或提前切页\n"); }
  simSaveBmp("boot_3_linked.bmp");

  // ---- 握手包（全字段无效）不能触发切页 ----
  //  PC 端连上后先发这个包确认协议通；如果按"收到包就切"处理，屏幕会先切到数据页
  //  显示一片 "--"，一秒后才出数 —— 正是启动画面要避免的画面。
  publishHandshake();
  simAdvanceMs(BOOT_FRAME_MS);
  loop();
  printf("  收到全无效握手包后：仍在启动画面=%d（期望 1）  detectPage=%d（期望 0）\n",
         (int)uiBooting(), detectPage());
  if (!uiBooting()) { bootBad++; printf("  !! 握手包就把屏幕切到了数据页（会闪一片 --）\n"); }

  // ---- 收到第一包数据：切到数据页面（概览页），并检查接缝里没有启动画面残留 ----
  publish(0);
  int bootFrames = 0;
  while (uiBooting() && bootFrames < 50) { simAdvanceMs(BOOT_FRAME_MS); loop(); bootFrames++; }
  printf("  收到数据后 %d 帧内切到数据页：uiBooting=%d（期望 0）  detectPage=%d（期望 1）\n",
         bootFrames, (int)uiBooting(), detectPage());
  if (uiBooting() || detectPage() != 1) { bootBad++; printf("  !! 收到数据后没有切到数据页面\n"); }
  // 数据页只覆盖「标题栏/两张卡片/中块/底栏」，卡片之间与四周的 4px 缝隙要靠
  // bootClearResidue() 补清；不补的话启动画面的白字会留在缝里。
  int gapPix = simCountColor(158, 26, 162, 138, 0x0000);
  int leftPix = simCountColor(0, 26, 4, 204, 0x0000);
  printf("  接缝残留检查：两卡之间 %d/%d 像素是背景，左边距 %d/%d（都期望相等）\n",
         gapPix, 4 * 112, leftPix, 4 * 178);
  if (gapPix != 4 * 112 || leftPix != 4 * 178) {
    bootBad++; printf("  !! 缝隙里有启动画面残留（bootClearResidue 没盖到）\n");
  }
  simSaveBmp("boot_4_data_after.bmp");

  printf("  %s\n", bootBad ? "  !! 开机画面有失败项" : "  开机画面全部通过");

#if FORCE_PAGE
  // 调试模式（config.h 的 FORCE_PAGE 非 0）：只验证"画面恒定停在这一页"
  printf("=== 调试模式：FORCE_PAGE=%d，只渲染该页 ===\n", FORCE_PAGE);
  for (int f = 0; f < 5; f++) {
    simAdvanceMs(200); publish(f); loop();
    printf("  frame %d: detectPage=%d  IO青(0x07FF)=%d  趋势底(0x0861)=%d  散热底槽(0x0841)=%d\n",
           f, detectPage(),
           simCountColor(12, 48, 150, 81, 0x07FF),
           simCountColor(26, 42, 312, 150, 0x0861),
           simCountColor(44, 142, 210, 204, 0x0841));
  }
  int badFrames = 0;
  for (int f = 0; f < 300; f++) {            // 60 秒：正常轮播会翻 6 次页
    simAdvanceMs(200); publish(100 + f); loop();
    if (detectPage() != FORCE_PAGE) badFrames++;
  }
  simSaveBmp("preview_forced.bmp");
  printf("  300 帧（60s）内偏离第 %d 页的帧数 = %d（期望 0）\n", FORCE_PAGE, badFrames);
  // 调试行：中块标题右侧的 "S<刻度> R<raw_down>/<raw_up>"，右对齐到 PK_GPU_R=308
  // （注意 PK_GPU_R 的注释写的是 307，实际算式是 162+154-8=308），
  // 字体 1 每字符 6px，"S100 R0/0" 9 字符 = 54px -> 左上角 (254,147)。
  // 仿真里数据是直接塞进 HardwareData 的（没走 protoDecodeData），所以 raw 恒为 0
  int dbgBad = simTextIntact("S100 R0/0", 254, 147, 8, 0xD69A);
  printf("  调试行 \"S100 R0/0\" 完好=%d（期望 0）\n", dbgBad);
  printf("  %s\n", (badFrames || dbgBad) ? "  !! 恒定页面失败" : "  恒定页面生效");
  return (badFrames || dbgBad) ? 1 : 0;
#endif

  bool seen[5] = {false, false, false, false, false};

  for (int f = 0; f < 190; f++) {
    simAdvanceMs(200);
    publish(f);
    loop();

    int pg = detectPage();
    if (!seen[pg]) {
      seen[pg] = true;
      printf("\n########## 首次进入第 %d 页（frame %d, millis=%lu）##########\n",
             pg, f, (unsigned long)millis());
      printf("  页码圆点校验：当前页实心白点像素=%d（期望 >0）\n", pageDotOk(pg));

      if (pg == 1) {
        simSaveBmp("page1_overview.bmp");
        simDumpRegion("P1 概览：全屏 4x4", 0, 0, 320, 240, 4);
        simDumpRegion("P1 极值卡 1x1", 0, 142, 320, 205, 2);
        printf("  概览页不应再有波形（标题栏以下 0x0861 像素=%d，期望 0）\n",
               simCountColor(0, 24, 320, 240, 0x0861));
        printf("  CPU hero 底槽跨度：");
        simRunRow("hero", 77, 0x0841);       // 进度条底槽应在 12..149
        // 第 3 行是轮播行（标签会变），不在这里校验
        static const char *const np[] = {"TEMP", "CLK", "TEMP", "VRAM",
                                         "LOAD", "TEMP", "RAM", "DISK", "FPS"};
        static const int pp[9][2] = {{12, 88}, {12, 106}, {170, 88}, {170, 106},
                                     {12, 162}, {12, 186}, {10, 218}, {114, 218}, {218, 218}};
        checkLabels(np, pp, 9);
      }

      if (pg == 2) {
        simSaveBmp("page2_thermal.bmp");
        simDumpRegion("P2 散热：全屏 4x4", 0, 0, 320, 240, 4);
        simDumpRegion("P2 温度对比卡 1x1", 0, 142, 320, 205, 2);
        printf("  温度对比条底槽跨度：");
        simRunRow("meter0", 150, 0x0841);
        printf("  数值右边界（期望 <= 149 / 307）\n");
        printf("    CPU FAN 行最右非卡片底像素 x=%d\n", simRightmostNonBg(38, 84, 150, 100, simCardBg()));
        printf("    GPU MARG 行最右非卡片底像素 x=%d\n", simRightmostNonBg(196, 102, 308, 118, simCardBg()));
        printf("    温度对比 CPU 行数值最右像素 x=%d\n", simRightmostNonBg(214, 145, 308, 161, simCardBg()));
        // 中块三条的标签会轮播，单独在轮播测试里校验
        static const char *const n2[] = {"FAN", "MARG", "\xF8" "C/W", "FAN", "MARG", "\xF8" "C/W",
                                         "UP", "PKTS", "RATE"};
        static const int p2[9][2] = {{12, 88}, {12, 106}, {12, 124}, {170, 88}, {170, 106}, {170, 124},
                                     {10, 218}, {114, 218}, {218, 218}};
        checkLabels(n2, p2, 9);
      }

      if (pg == 3) {
        simSaveBmp("page3_trend.bmp");
        simDumpRegion("P3 趋势：全屏 4x4", 0, 0, 320, 240, 4);
        printf("  大图绘制区底(行)  ");
        simRunRow("plot", 100, 0x0861);
        printf("  大图绘制区底(列)  ");
        simRunCol("plot", 170, 0x0861);
        printf("  大图最右侧列(x=311) CPU 绿=%d GPU 橙=%d（>0 = 最新点贴右边缘）\n",
               simCountColor(311, 42, 312, 154, 0x7FE0), simCountColor(311, 42, 312, 154, 0xFB00));
        simDumpRegion("P3 页脚两行 1x1", 8, 158, 312, 200, 2);
        simDumpRegion("P3 底栏 1x1", 0, 210, 320, 236, 1);
        static const char *const n3[] = {"PEAK", "AVG", "FPS", "TIME", "LOW"};
        static const int p3[5][2] = {{12, 166}, {12, 185}, {10, 218}, {114, 218}, {218, 218}};
        checkLabels(n3, p3, 5);
      }

      if (pg == 4) {
        simSaveBmp("page4_io.bmp");
        simDumpRegion("P4 IO/NET：全屏 4x4", 0, 0, 320, 240, 4);
        simDumpRegion("P4 两张卡片 1x1", 0, 44, 320, 140, 2);
        printf("  DISK 卡片主指标条青色像素=%d（期望 >30：条是青色的，长度取决于速率）\n",
               simCountColor(12, 74, 150, 81, 0x07FF));
        simRunCol("hero", 30, 0x0841);
        static const char *const n4[] = {"READ", "WRITE", "USED", "UP", "TOTAL", "LOSS",
                                         "RATE", "UP", "PKTS", "PING", "MEM", "PROC", "FPS"};
        static const int p4[13][2] = {{12, 88}, {12, 106}, {12, 124}, {170, 88}, {170, 106}, {170, 124},
                                      {12, 162}, {12, 186}, {160, 162}, {160, 186},
                                      {10, 218}, {114, 218}, {218, 218}};
        checkLabels(n4, p4, 13);
        // 副标题：IO 页左卡片是 DISK，副标题必须是 STORAGE，不能是 CPU 型号名
        // （副标题在 (44,34)，字体 1、颜色 C_LABEL_DIM=0xD69A）
        int subOk  = simTextIntact("STORAGE", 44, 34, 8, 0xD69A);
        int subBad = simTextIntact("i5-12400", 44, 34, 8, 0xD69A);
        printf("  DISK 副标题：STORAGE 完好=%d（期望 0）  i5-12400 完好=%d（期望 >0）\n",
               subOk, subBad);
        printf("  %s\n", (subOk == 0 && subBad > 0) ? "  DISK 副标题正确"
                                                    : "  !! DISK 副标题被 CPU 型号名占了");
      }
    }
  }

  printf("\n--- 翻页结构 ---\n");
  printf("  四页都出现过：概览=%d 散热=%d 趋势=%d IO=%d（期望 1 1 1 1）\n",
         seen[1], seen[2], seen[3], seen[4]);

  // ---------------- 频闪修复验证 ----------------
  printf("\n--- 行轮播验证（每 ROT_MS=5s=25 帧 换一档）---\n");
  // 注意：观测前必须先确认还停在概览页——页周期 9s 比一次观测+等待还短，
  // 中途翻页会看到别的页面的行（这个坑踩过一次）
  for (int step = 0; step < 3; step++) {
    int pgR = detectPage();
    for (int f = 0; pgR != 1 && f < 220; f++) {   // 四页一轮 36s，等回概览最多要 36s
      simAdvanceMs(200); publish(600 + step * 40 + f); loop(); pgR = detectPage();
    }
    int bPwr = simLabelIntact("PWR", 12, 124);
    int bVc  = simLabelIntact("VCORE", 12, 124);
    int bCr  = simLabelIntact("CORE", 12, 124);
    const char *cpuLbl = (bPwr == 0) ? "PWR" : (bVc == 0) ? "VCORE" : (bCr == 0) ? "CORE" : "??";
    int hits = (bPwr == 0) + (bVc == 0) + (bCr == 0);
    int gPwr = simLabelIntact("PWR", 170, 124);
    int gGc  = simLabelIntact("GCLK", 170, 124);
    int gVm  = simLabelIntact("VMEM", 170, 124);
    const char *gpuLbl = (gPwr == 0) ? "PWR" : (gGc == 0) ? "GCLK" : (gVm == 0) ? "VMEM" : "??";
    int gHits = (gPwr == 0) + (gGc == 0) + (gVm == 0);
    printf("  第 %d 档（页=%d）：CPU 第3行=%-5s（命中 %d/3）  GPU 第3行=%-5s（命中 %d/3）\n",
           step, pgR, cpuLbl, hits, gpuLbl, gHits);
    if (hits != 1) { printf("    !! CPU 轮播行标签异常\n"); simInkMap("实际", 4, 119, 60, 137); }
    if (gHits != 1) printf("    !! GPU 轮播行标签异常\n");
    for (int f = 0; f < 25; f++) { simAdvanceMs(200); publish(900 + step * 30 + f); loop(); }
  }

  // ---------------- 合法 0 值：协议哨兵不能把 0 吞掉 ----------------
  printf("\n--- 合法 0 值验证（0 MB/s 必须显示 0，不能显示 --）---\n");
  int pg0 = detectPage();
  for (int f = 0; pg0 != 4 && f < 220; f++) {
    simAdvanceMs(200); publish(1200 + f); loop(); pg0 = detectPage();
  }
  printf("  当前页=%d（期望 4 IO 页）\n", pg0);
  g_data.diskRead = 0.0f; g_data.diskWrite = 0.0f; g_data.netDown = 0.0f;
  g_data.netUp = 0.0f; g_data.pingLoss = 0.0f; g_data.diskUsed = 0.0f;
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int xZero = simLeftmostNonBg(80, 84, 150, 100, simCardBg());
  printf("  diskRead=0 时 READ 行最左墨迹 x=%d（\"0KB/s\" 约从 112 起）\n", xZero);
  // 再验证哨兵：读不到时必须显示 --（"--" 没有单位、又只有 2 字符，最左墨迹会明显右移）
  g_data.diskRead = -1.0f;
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int xNa = simLeftmostNonBg(80, 84, 150, 100, simCardBg());
  printf("  diskRead=哨兵 时 READ 行最左墨迹 x=%d（\"--\" 约从 126 起，必须比 0 更靠右）\n", xNa);
  printf("  %s\n", (xZero < xNa) ? "  合法 0 与哨兵区分正确" : "  !! 0/哨兵区分不正确");

  // ---------------- 磁盘速率也走同一套单位规则 ----------------
  // READ 行的单位画在 xRight=CPU_X+CARD_W-CARD_PAD=150、y=ROW_DY(0) 行内 +8 处，
  // 字体 1 每字符 6px -> "KB/s"/"MB/s" 24px 宽 -> 左上角 (126, 92)
  g_data.diskRead = 0.5f;                     // 512 KB/s
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int dKb = simLabelIntactN("KB/s", 126, 92, 8);
  g_data.diskRead = 2.0f;                     // 2.0 MB/s
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int dMb = simLabelIntactN("MB/s", 126, 92, 8);
  printf("  磁盘 READ：0.5MB/s->KB/s 完好=%d（期望 0）  2.0MB/s->MB/s 完好=%d（期望 0）\n", dKb, dMb);
  printf("  %s\n", (dKb == 0 && dMb == 0) ? "  磁盘速率单位同样自适应" : "  !! 磁盘速率单位异常");

  // ---------------- 网络速率单位自适应：<1 MB/s 必须显示 KB/s ----------------
  // 空闲网络只有几~几十 KB/s，显示成 "0.0 MB/s" 就是一排 0，像没数据。
  // 单位画在 NET 卡片主指标区右下角：x = GPU_X+CARD_W-CARD_PAD = 308，
  // y = CARD_Y+HERO_DY+HERO_H-8 = 66；字体 1 每字符 6px，"KB/s"/"MB/s" 都 24px 宽
  // -> 左上角 (284,66)；字形只有 8 行，用 simLabelIntactN(...,8) 避开 y=75 的进度条底槽。
  // ---------------- 网络单位：MB/s / KB/s（和磁盘同一套规则）----------------
  // 单位画在 NET 卡片主指标区右下角：xr = GPU_X+CARD_W-CARD_PAD = 308，
  // y = CARD_Y+HERO_DY+HERO_H-8 = 66；字体 1 每字符 6px -> 左上角 (284,66)。
  g_data.netDown = 0.084f;                    // 86 KB/s
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int netKb    = simLabelIntactN("KB/s", 284, 66, 8);
  int netMbBad = simLabelIntactN("MB/s", 284, 66, 8);
  // 扫描窗口只取大字那 26 行（y 48..73）：跳过 x<200 的 "DOWN" 标签，
  // 也跳过 y=75 起的进度条底槽，否则最左墨迹永远是窗口左边界。
  int xl86 = simLeftmostNonBg(200, 48, 308, 74, simCardBg());
  printf("  netDown=0.084MB/s：KB/s 完好=%d（期望 0）  MB/s 完好=%d（期望 >0）\n", netKb, netMbBad);
  printf("    主指标墨迹最左 x=%d（\"86\" 期望 %d±2）\n", xl86, 282 - w4("86"));
  g_data.netDown = 3.07f;                     // 3.1 MB/s
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int netMb = simLabelIntactN("MB/s", 284, 66, 8);
  int xl31 = simLeftmostNonBg(200, 48, 308, 74, simCardBg());
  printf("  netDown=3.07MB/s：MB/s 完好=%d（期望 0）  主指标墨迹最左 x=%d（\"3.1\" 期望 %d±2）\n",
         netMb, xl31, 282 - w4("3.1"));
  printf("  %s\n", (netKb == 0 && netMbBad > 0 && netMb == 0 &&
                    abs(xl86 - (282 - w4("86"))) <= 2 && abs(xl31 - (282 - w4("3.1"))) <= 2)
                    ? "  网络 MB/s·KB/s 显示正确" : "  !! 网络单位异常");

  // 1 MB/s 分界：0.99 -> 1014 KB/s（走 KB/s）；1.00 -> 1.0 MB/s（走 MB/s）
  g_data.netDown = 0.99f;
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int kb99 = simLabelIntactN("KB/s", 284, 66, 8);
  g_data.netDown = 1.00f;
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int mb100 = simLabelIntactN("MB/s", 284, 66, 8);
  printf("  分界：0.99MB/s->KB/s 完好=%d  1.00MB/s->MB/s 完好=%d（都期望 0）\n", kb99, mb100);
  printf("  %s\n", (kb99 == 0 && mb100 == 0) ? "  1 MB/s 分界正确" : "  !! 1 MB/s 分界不对");

  printf("\n--- 频闪修复验证 ---\n");
  printf("  fillScreen 调用次数=%d（期望 1：只有 screenInit 清过一次，切页不清屏）\n",
         simFillScreenCalls());

  // 稳态帧：数据在变（会触发数值刷新），但没切页、没变状态
  // -> 屏幕上不该有任何文字绘制（都走离屏画布），否则就是"先擦后画"的微闪来源
  simResetCounters();
  simAdvanceMs(200); publish(60); loop();       // 60: 温度 54 度，不会触发告警打断测试
  printf("  稳态帧直接画到屏幕的文字次数=%d（期望 0，说明局部刷新全走离屏画布）\n",
         simTextOnScreen());

  // ---------------- 回到概览页时的几何断言 ----------------
  int pg = detectPage();
  for (int f = 0; pg != 1 && f < 120; f++) {   // 一页 45 帧，从任意页等回概览最多两页
    simAdvanceMs(200); publish(400 + f); loop();
    pg = detectPage();
  }
  printf("\n--- 当前页 = %d（应为概览页 1）---\n", pg);
  if (pg == 1) {
    simBBox("CPU 卡片描边", 0x0460);
    simBBox("GPU 卡片描边", 0x8800);
    printf("    概览页波形残留(0x0861, 标题栏以下)=%d（期望 0：这一页不该有曲线图）\n",
           simCountColor(0, 24, 320, 240, 0x0861));
    printf("    CPU TEMP 行最右非卡片底像素 x=%d（期望 <=149）\n", simRightmostNonBg(38, 84, 150, 100, simCardBg()));
    printf("    GPU VRAM 行最右非卡片底像素 x=%d（期望 <=307）\n", simRightmostNonBg(196, 102, 308, 118, simCardBg()));
    printf("    极值卡 LOAD 行最右像素 x=%d（期望 <=307）\n", simRightmostNonBg(46, 158, 308, 174, simCardBg()));
    printf("    底栏 RAM/DISK/FPS 最右像素 x=%d/%d/%d（期望 <=100/204/308）\n",
           simRightmostNonBg(34, 214, 100, 230, simCardBg()),
           simRightmostNonBg(138, 214, 204, 230, simCardBg()),
           simRightmostNonBg(242, 214, 308, 230, simCardBg()));
  }

  // ---------------- 告警锁定：温度告警应跳到"能看到温度"的散热页 ----------------
  printf("\n--- 温度告警锁定 ---\n");
  for (int f = 0; f < 120; f++) {            // 先喂正常数据让告警解除、轮播恢复，等到趋势页
    simAdvanceMs(200); publish(200 + f); loop();
    if (detectPage() == 3) break;
  }
  printf("  制造告警前所在页 = %d（期望 3 趋势页）\n", detectPage());
  g_data.cpuTemp = 96.0f;                    // 触发 CPU 温度告警
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  printf("  注入 96 度后所在页 = %d（期望 2 散热页）\n", detectPage());
  // 边框呼吸有相位，多跑几帧取"出现过红色"为准
  int borderRed = 0;
  for (int f = 0; f < 4; f++) {
    simAdvanceMs(200); loop();
    int r = simCountColor(4, 26, 8, 138, 0xF800) + simCountColor(162, 26, 166, 138, 0xF800);
    if (r > borderRed) borderRed = r;
  }
  printf("  告警中卡片边框红像素峰值=%d（期望 >0：边框呼吸而不是数字闪）\n", borderRed);
  simAdvanceMs(200); loop();
  printf("  再等一帧仍在页 = %d（期望 2，告警期间不轮播）\n", detectPage());

  printf("\n--- 告警解除（96 -> 60 度）---\n");
  g_data.cpuTemp = 60.0f;
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  printf("  温度大字区 CPU 绿像素=%d（期望 >0）  红色像素=%d（期望 0）\n",
         simCountColor(12, 48, 150, 81, 0x7FE0), simCountColor(12, 48, 150, 81, 0xF800));

  // ---------------- 负载告警：应跳到概览页 ----------------
  printf("\n--- 负载告警锁定 ---\n");
  g_data.cpuLoad = 99.0f;
  g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  printf("  注入 99%% 负载后所在页 = %d（期望 1 概览页）\n", detectPage());
  printf("  CPU 负载大字区红色像素=%d（期望 >0）\n", simCountColor(12, 48, 150, 81, 0xF800));

  // ---------------- 断线：置灰 + NO SIGNAL（趋势页才有曲线图）----------------
  printf("\n--- 断线帧 ---\n");
  g_data.cpuLoad = 40.0f;                    // 先让负载告警解除，恢复轮播
  int pg3 = detectPage();
  for (int f = 0; pg3 != 3 && f < 120; f++) {
    simAdvanceMs(200); publish(400 + f); loop();
    pg3 = detectPage();
  }
  printf("  断线前所在页 = %d（期望 3 趋势页）\n", pg3);
  for (int f = 0; f < 20; f++) { simAdvanceMs(200); loop(); }
  printf("  断线后所在页 = %d（不再锁定，停在当前页）\n", detectPage());
  printf("  大图 NO SIGNAL 红像素=%d（期望 >0）\n", simCountColor(26, 42, 312, 154, 0xF800));
  printf("  大图内曲线残留 CPU 绿=%d GPU 橙=%d（期望 0）\n",
         simCountColor(26, 42, 312, 154, 0x7FE0), simCountColor(26, 42, 312, 154, 0xFB00));
  printf("  置灰数值(0x8410)像素=%d（期望 >0）\n", simCountColor(0, 0, 320, 240, 0x8410));
  simSaveBmp("preview_stale.bmp");

  // ---------------- 告警锁页超时：告警一直不解除，也必须恢复轮播 ----------------
  // （旧行为：任何一个告警长期存在就把画面永久钉死，NVMe/HX 版 CPU 温度很容易触发）
  printf("\n--- 告警锁页超时（ALARM_PIN_MS=%lums）---\n", (unsigned long)ALARM_PIN_MS);
  // "持续告警"必须每帧都喂新包：DATA_TIMEOUT_MS=3s，一帧不喂就判断线，
  // stale 之后 anyAlarm 直接为假，锁页会提前结束，测不到超时逻辑。
  publish(700); g_data.cpuTemp = 96.0f; g_seq++; g_lastMs = millis();
  simAdvanceMs(200); loop();
  int pgPin = detectPage();
  printf("  注入 96 度后所在页 = %d（期望 2 散热页：告警跳页）\n", pgPin);
  for (int f = 0; f < 50; f++) {                 // +10s，还在 12s 锁页期内
    publish(701 + f); g_data.cpuTemp = 96.0f;
    simAdvanceMs(200); loop();
  }
  int pgHold = detectPage();
  printf("  告警持续 10.4s 后所在页 = %d（期望 2：锁页期内不轮播）\n", pgHold);
  for (int f = 0; f < 15; f++) {                 // 越过 12s 锁页期
    publish(760 + f); g_data.cpuTemp = 96.0f;
    simAdvanceMs(200); loop();
  }
  int pgAfter = detectPage();
  printf("  告警持续 13.4s 后所在页 = %d（期望 != 2：超时后恢复轮播）\n", pgAfter);
  int toThermal = 0;                             // 转回来时告警视觉必须还在
  for (int f = 0; f < 200 && detectPage() != 2; f++) {
    publish(780 + f); g_data.cpuTemp = 96.0f;
    simAdvanceMs(200); loop(); toThermal++;
  }
  int redHero = simCountColor(12, 48, 150, 81, 0xF800);
  printf("  轮播 %d 帧后回到散热页；96 度大字红像素=%d（期望 >0：告警视觉保留）\n",
         toThermal, redHero);
  printf("  结论：%s\n",
         (pgPin == 2 && pgHold == 2 && pgAfter != 2 && redHero > 0) ? "通过" : "失败");

  simSaveBmp("preview_live.bmp");
  printf("\nwaveIdx=%u waveCount=%u\n", waveIdx, waveCount);
  return 0;
}
