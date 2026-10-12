#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>   // uint16_t（本文件不依赖包含顺序）

// 屏幕引脚 / 驱动 / 分辨率等硬件配置统一放在 tft_setup.h（本 sketch 目录下），
// 由 TFT_eSPI 自动加载；此处不再重复定义，避免两处数值不一致。
// TFT_BL 等宏在 tft_setup.h 中定义后，包含 <TFT_eSPI.h> 的文件可直接使用。

// BLE UUID
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_WRITE_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_DEVICE_NAME     "PC-MONITOR-ST7789"

// BLE MTU 配置
#define BLE_MTU_SIZE 128

// 波形与刷新配置
#define WAVE_POINTS 140
#define REFRESH_DELAY 200

// ---------------- 开机画面（连上 PC 之前显示的东西）----------------
// 上电后先显示启动画面（大标题 + 扫描条 + 实时状态），直到 **BLE 连上并且收到
// 第一包数据** 才切到数据页面。这样"PC 端程序还没启动"时屏幕上不会是一片
// "--" 加 NO SIGNAL（那看着像坏了），而是明确地写着在等什么。
#define BOOT_FRAME_MS   33       // 启动画面的帧间隔（约 30fps；数据页仍是 REFRESH_DELAY）
#define BOOT_MIN_MS     900UL    // 启动画面至少显示这么久，避免"PC 端秒连"时只闪半帧
#define BOOT_HINT_MS    15000UL  // 超过这么久还没连上，追加一行 "START THE PC APP ON THIS PC"

// ---------------- 告警阈值（UI 用；改这里就能调敏感度）----------------
// 所有阈值都带回差 ALARM_HYS：超过触发值才报警，掉到 (触发值 - 回差) 以下才解除，
// 避免读数在阈值附近抖动时数值疯狂闪烁。
#define ALARM_CPU_TEMP   95.0f   // HX 系移动版满载常态 90~100 度，定 90 会整天响不停
#define ALARM_GPU_TEMP   85.0f
#define ALARM_GPU_HOT   100.0f   // GPU 热点（Hot Spot 本来就比核心高十几度）
#define ALARM_VRAM_TEMP  95.0f   // 显存结温
#define ALARM_HDD_TEMP   70.0f   // NVMe 固态常态 55~70 度（旧的 55 是机械硬盘标准）
#define ALARM_CPU_LOAD   95.0f
#define ALARM_GPU_LOAD   95.0f
#define ALARM_MEM        90.0f
#define ALARM_HYS         3.0f
#define BLINK_MS          500UL   // 告警时卡片边框呼吸的半周期（ms）

// 进度条/状态色分档（绿 -> 黄 -> 红）
#define BAR_WARN_PCT     60.0f
#define BAR_CRIT_PCT     85.0f

// 数据超时：超过这么久没收到新包，判定为"链路无数据"
// -> 数值置灰、趋势图显示 NO SIGNAL
#define DATA_TIMEOUT_MS  3000UL

// 界面自动轮播：每页停留时长（概览 / 散热 / 趋势 / IO·网络）
// 出现告警会跳到"能看到该指标的那一页"提醒一下，最多钉 ALARM_PIN_MS；
// 超时后即使告警没解除也恢复轮播（数值常红 + 边框呼吸照旧，不会漏看）。
// **但刚连上后的第一个 PAGE_MS 里不跳页**：先让概览页完整显示一轮 —— 连上的
// 那一刻往往温度已经越线（刚打完游戏），否则用户刚看完开机画面就被拉去散热页。
// 另外：**整页都是 "--"（该页指标全读不到）的页会被跳过**，不翻过去；
// 一页可看的都没有就停在当前页不翻（判定见 screen_draw.cpp 的 pageHasData）。
#define PAGE_MS          9000UL
#define ALARM_PIN_MS    12000UL   // 告警锁页最长停留（ms）

// ---------------- 调试开关：恒定显示某一页 ----------------
//   0 = 正常轮播；1 概览 / 2 散热 / 3 趋势 / 4 IO·网络
// 这里的编号就是屏幕上的页码（跟标题栏圆点一致，1 基），内部会自己减 1。
// 非 0 时画面钉死在该页：不轮播、也不被告警跳走（告警配色和边框呼吸照旧），
// 用来盯某个读数（比如调网络速率），同时中块右边会多一行 "S<刻度> R<raw>" 调试字。
// 测完改回 0。离线仿真编译时用 -DFORCE_PAGE=0 覆盖，否则翻页类断言全部失效。
#ifndef FORCE_PAGE
#define FORCE_PAGE      0
#endif

// 波形最小推送间隔：sprite 一次推送是 3~9ms 的逐行写入，
// 更新太密会看成"从上往下刷"的微闪，所以限制到 2.5Hz 以内。
#define TREND_MIN_MS     400UL

// 行轮播周期：同一个位置轮流显示几个指标，用来在 320x240 上放下更多数据
// （电压/热点/显存温度/GPU频率/最忙核心/机箱风扇这些就走轮播）
#define ROT_MS           5000UL

// 硬件数据结构体：32 个字段，顺序与 PROTOCOL.md 的字段表一致
// 二进制定标编码（×1 / ×10 / ×100），0xFFFF 表示"读不到"（内部存 -1）
struct HardwareData {
  float cpuLoad;      // %
  float cpuTemp;      // °C
  float cpuFan;       // RPM
  float cpuPower;     // W
  float gpuLoad;      // %
  float gpuTemp;      // °C
  float gpuMem;       // GB 已用
  float gpuFan;       // RPM
  float gpuPower;     // W
  float fps;          // 帧率（实测）
  float memUsed;      // %
  float hddTemp;      // °C
  float gpuMemTotal;  // GB 总量
  float cpuClk;       // MHz
  float cpuVolt;      // V   CPU 核心电压
  float cpuCoreMax;   // %   最忙核心
  float gpuHotspot;   // °C  GPU 热点
  float gpuMemTemp;   // °C  GPU 显存温度
  float gpuClk;       // MHz GPU 核心频率
  float gpuMemCtrl;   // %   显存控制器负载
  float moboFan;      // RPM 机箱/主板风扇
  float diskRead;     // MB/s
  float diskWrite;    // MB/s
  float diskUsed;     // %
  float netDown;      // MB/s
  float netUp;        // MB/s
  float pingMs;       // ms
  float pingLoss;     // %
  float procCount;    // 个
  float uptimeMin;    // 分钟
  float frameMs;      // ms 帧时间
  float fpsLow;       // fps 1% low（最慢的 1% 帧的平均帧率，PC 端算好送来）
};

// 外部全局变量声明
extern int cpuWave[WAVE_POINTS];
extern int gpuWave[WAVE_POINTS];
// 注：原来还有一个 fpsWave（帧率历史），它唯一的用途是屏幕自己算"窗口内最低帧率"。
// 1% low 改成由 PC 端在逐帧数据上算好送来（协议字段 fps_low）之后，
// 那个数组和它的统计函数都成了死代码，已删除。
extern uint16_t waveIdx;        // 环形缓冲写入位置（下一个待写槽位）
extern uint16_t waveCount;      // 已写入的总样本数，封顶 WAVE_POINTS（判断波形是否写满）
extern HardwareData dataPack;   // 只能用 ble_service 的快照 API 读写，不要直接访问
extern volatile bool bleConnected;

#endif
