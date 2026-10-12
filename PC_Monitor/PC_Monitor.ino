#include "config.h"
#include "screen_draw.h"
#include "ble_service.h"

// 全局变量定义
int cpuWave[WAVE_POINTS] = {0};
int gpuWave[WAVE_POINTS] = {0};
uint16_t waveIdx = 0;
uint16_t waveCount = 0;
HardwareData dataPack;
volatile bool bleConnected = false;

// 推进波形环形缓冲。
// 放在主循环而不是 BLE 回调里：回调跑在蓝牙任务，和绘制任务并发读写同一个数组
// 会画出错乱的尖刺；这里和 drawMainUI 同任务，天然安全。
// 代价：如果 PC 端发包比主循环(200ms/5Hz)还快，多余的包会被合并（趋势图最多 5 点/秒，够看趋势）。
// 注：原来这里还推一个 fpsWave，用于屏幕自己算"窗口内最低帧率"。
// 1% low 改由 PC 端在逐帧数据上算好送来（协议字段 fps_low），那个数组已删除。
static void wavePush(const HardwareData &d) {
  cpuWave[waveIdx] = (int)constrain(d.cpuLoad, 0, 100);
  gpuWave[waveIdx] = (int)constrain(d.gpuLoad, 0, 100);
  waveIdx = (waveIdx + 1) % WAVE_POINTS;
  if (waveCount < WAVE_POINTS) waveCount++;
}

void setup() {
  Serial.begin(115200);
  screenInit();      // 先建离屏缓冲：此时蓝牙还没吃内存，堆更整块
  bleInit();

  // 波形页的离屏缓冲是 286x120x2 ≈ 68KB，这里确认蓝牙起来之后还剩多少堆
  Serial.printf("System Ready, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  HardwareData snap;
  uint32_t seq;
  if (bleDataTake(snap, seq)) wavePush(snap);   // 只有真收到新包才推进波形

  drawMainUI();

  // 启动画面要 30fps 才像动画；连上之后数据页 5Hz 就够（也没必要一直推大块 SPI）
  delay(uiFrameMs());
}
