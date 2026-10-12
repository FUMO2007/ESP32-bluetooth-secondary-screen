# ui_preview —— 不上板就能看界面的离线仿真

把 `PC_Monitor/screen_draw.cpp` 和 `PC_Monitor/PC_Monitor.ino`（**真实代码，不做任何拷贝**）
跑在一套内存帧缓冲的 TFT_eSPI 桩上，喂假数据渲染出整屏，并做像素级几何断言。
用于改 UI 时先在这里确认布局，再烧板子。

## 怎么跑（在项目根目录 PC_Monitor_1 下执行）

> **推荐直接用仓库根目录的一键脚本**，它会把固件语法检查、协议跨语言测试、
> PC 端逻辑测试和本仿真全部跑一遍，**并且检查每一步的退出码**：
>
> ```powershell
> .\build.cmd          # 全部（推荐入口，见下方"注意"）
> .\build.cmd -Quick   # 跳过本仿真
>
> # 等价写法（可能被执行策略拦下，见"注意"）：
> powershell -NoProfile -File build.ps1          # 全部
> powershell -NoProfile -File build.ps1 -Quick   # 跳过本仿真
> ```
>
> **注意（本机实测）：** 直接跑 `powershell -File build.ps1` 可能报
> `The file ... is not digitally signed` —— 本机有活动的代码完整性策略，
> **新建或被编辑过的 .ps1 会被拒绝执行**，与脚本内容无关（一直没动过的
> `deploy.ps1` 能跑，它的字节副本却不能）。`build.cmd` 已经带上
> `-ExecutionPolicy Bypass`，直接用它即可；原因与验证写在 `build.ps1` 头部。
>
> 产物落在 `build_out/`（不再覆盖仓库里已提交的 exe / bmp）。
>
> 脚本额外做了一件下面手抄的命令做不到的事：`preview.exe` **正常路径永远返回 0**
> （`preview_main.cpp` 的 `main()` 无条件 `return 0`，只有 `#if FORCE_PAGE` 分支
> 会返回失败码），所以它的退出码没有信息量。脚本改为读 `report.txt`，按本文件
> 的约定把任何 `!!` 开头的行和 `结论：失败` 当作失败。
>
> 下面是不用脚本时的手抄命令，仍然有效：

```powershell
# 1) 只做语法/类型检查（不需要跑起来）
g++ -std=gnu++11 -fsyntax-only -Wall -Wextra -I tools\ui_preview -I PC_Monitor PC_Monitor\screen_draw.cpp
g++ -std=gnu++11 -fsyntax-only -Wall -Wextra -I tools\ui_preview -I PC_Monitor PC_Monitor\ble_service.cpp
g++ -std=gnu++11 -fsyntax-only -Wall -Wextra -x c++ -I tools\ui_preview -I PC_Monitor PC_Monitor\PC_Monitor.ino

# 2) 渲染 + 断言（一定要带 -DFORCE_PAGE=0：config.h 里调试开关默认可能钉死某一页，
#    那样翻页类断言会全部失效）
g++ -std=gnu++11 -O0 -DFORCE_PAGE=0 -I tools\ui_preview -I PC_Monitor tools\ui_preview\preview_impl.cpp `
    tools\ui_preview\preview_main.cpp PC_Monitor\screen_draw.cpp PC_Monitor\proto.cpp `
    -x c++ PC_Monitor\PC_Monitor.ino -o tools\ui_preview\preview.exe
tools\ui_preview\preview.exe | Out-File -Encoding utf8 tools\ui_preview\report.txt

# 3) 想验证"恒定页面"调试开关本身，就去掉 -DFORCE_PAGE=0（用 config.h 的值）
g++ -std=gnu++11 -O0 -I tools\ui_preview -I PC_Monitor tools\ui_preview\preview_impl.cpp `
    tools\ui_preview\preview_main.cpp PC_Monitor\screen_draw.cpp PC_Monitor\proto.cpp `
    -x c++ PC_Monitor\PC_Monitor.ino -o tools\ui_preview\preview_forced.exe
tools\ui_preview\preview_forced.exe      # 只跑"帧数内不偏离该页"这一项断言
```

> 注意 `-I tools\ui_preview` 必须在 `-I PC_Monitor` 之前/同时给出：桩头文件
> （`Arduino.h`、`TFT_eSPI.h`…）就是靠这个路径顶替真实库的。

产物：

- `boot_1_waiting.bmp` / `boot_2_hint.bmp` / `boot_3_linked.bmp` / `boot_4_data_after.bmp`：
  开机画面（等 PC / 超时提示 / 已连接等数据）与刚切进数据页的第一帧
- `page1_overview.bmp` / `page2_thermal.bmp` / `page3_trend.bmp` / `page4_io.bmp`：四个页面各一张（BMP，可直接看）
- `preview_live.bmp` / `preview_stale.bmp`：最后停在概览页的正常帧 / 断线无数据帧
- `preview_nodata.bmp`：所有指标都读不到的那一帧（"整页 `--` 就不再翻过去"的样本）
- `report.txt`：字符化的画面 + 全部断言结果

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `Arduino.h` / `TFT_eSPI.h` / `BLEDevice.h` 等 | 桩头文件，**签名照抄** `libraries/` 与 esp32 core 3.3.10 里的真实库 |
| `fonts_gen.h` | 从 `libraries/TFT_eSPI/Fonts/` 提取的 GLCD 点阵与 Font16/Font32 宽度表 |
| `preview_impl.cpp` | 桩实现：帧缓冲、图元、文本、sprite、BMP 导出、字符化/墨迹图/断言工具 |
| `preview_main.cpp` | 假数据源 + 主流程 + 断言清单 |
| `proto_test.cpp` | **协议跨语言测试**：解 `pc_side/test_proto.py` 生成的金标向量 |
| `page*.png` / `preview_*.png` / `boot_*.png` | 已渲染好的预览图（4 个页面、断线帧、开机画面三个阶段） |

编译仿真（三个可执行文件）：

```powershell
g++ -std=gnu++11 -O0 -I tools\ui_preview -I PC_Monitor tools\ui_preview\preview_impl.cpp `
    tools\ui_preview\preview_main.cpp PC_Monitor\screen_draw.cpp PC_Monitor\proto.cpp `
    -x c++ PC_Monitor\PC_Monitor.ino -o tools\ui_preview\preview.exe
g++ -std=gnu++11 -I tools\ui_preview -I PC_Monitor tools\ui_preview\proto_test.cpp `
    PC_Monitor\proto.cpp -o tools\ui_preview\proto_test.exe
```

## 断言覆盖了什么

- **开机画面**（第 0 段，先于所有页面断言）：上电后处于启动画面而不是数据页
  （`uiBooting()` 为真、`detectPage()` 返回 0）；品牌位图（"奶娃科技"）的白墨左右居中
  （实测宽 131px、偏差 1px）；
  扫描条的高亮逐帧在动（10 帧里至少 6 个不同位置）且始终在底槽内；
  等 PC 时状态行是黄的、连上后转青；超过 `BOOT_HINT_MS` 出现 "START THE PC APP ON THIS PC"；
  稳态帧"直接画到屏幕的文字次数=0"（动画也走离屏画布）；
  收到第一包数据后 1 帧内切到概览页，且**数据页盖不到的 4px 缝隙里没有启动画面残留**
  （两卡之间 448/448、左边距 712/712 像素都是背景 —— 少了 `bootClearResidue()` 就会红）。
- **整页都是 "--" 的页不轮播过去**（用户要求）：把 IO 页的指标全置成哨兵后，
  50 秒（约 5.5 个轮播周期）内**一次都没有停在 IO 页**，而概览/散热/趋势三页都正常出现；
  把全部指标都置成哨兵后，26 秒内**一次页都没换**（停在原地，不为了轮播而轮播）。
  判页用标题栏页码圆点（`detectDotPage()`）而不是 `detectPage()` —— 整页是 `--` 时
  那些颜色特征（IO 页的青色大字等）根本不会出现，`detectPage()` 会退化成返回 1。
- **翻页**：四页都出现、页码圆点指向当前页；温度告警自动跳到散热页、负载告警跳到概览页；
  **锁页只持续 `ALARM_PIN_MS`（12s）**，超时后即使告警没解除也恢复轮播（否则长期超阈值的
  指标会把画面永久钉死）；转回来时告警视觉（红色数值 + 边框呼吸）仍在；告警解除后颜色
  恢复；断线后不再锁定。
- **行轮播**：概览页 CPU/GPU 卡片第 3 行每 5 秒在 `PWR↔VCORE↔CORE` / `PWR↔GCLK↔VMEM`
  之间切换，断言"恰好命中一个候选标签"。
- **几何**：卡片描边精确 `x4..157 / x162..315`、趋势页大图 `x26..311 y42..153`、
  数值右边界不超过槽位、曲线贴右边缘。
- **概览页没有曲线图**：标题栏以下不允许出现波形底色 `0x0861`（这一页只放数值）。
- **速率单位**：所有 MB/s 类读数（磁盘 R+W/READ/WRITE、网络 DOWN/UP/TOTAL）统一遵守
  "`>=1 MB/s` 用 MB/s、`<1 MB/s` 用 KB/s"（含 0.99 / 1.00 的 1 MB/s 分界）。
  断言用 `simLabelIntactN`（可指定比对行数，避开贴着的进度条底槽）逐像素比对单位文字，
  并用 `w4()`（字体 4 的真实宽度表）反推主指标大字的墨迹起点来核对数值本身；
  同时确认合法 0（显示 `0KB/s`）与哨兵（显示 `--`）仍能区分。
- **压字**：每个标签做**逐像素比对**（把期望标签单独渲染到空白缓冲再比），
  任何数值压到标签都会报出像素数。
- **频闪**（这轮新加的，两处都是硬指标）：
  - `fillScreen 调用次数 == 1`：切页不许清屏（清屏就是每 9 秒一次的整屏闪）。
  - 稳态帧"直接画到屏幕的文字次数 == 0"：所有会变的数值都必须走离屏画布再整块推送，
    否则就是"先擦后画"的 1~2ms 空窗，1~5Hz 连续刷新看起来就是频闪。
- **残留**：切页后检查上一页内容有没有露在缝里（圆角卡片缺口、卡片之间的 4px 缝隙）——
  趋势页的大卡片是实心覆盖的，切回去时这两处最容易留下"一条横线"。
- **状态**：断线时数值置灰 + `NO SIGNAL` + 曲线无残留；FPS=0 显示中性灰不报警。

## 已知的仿真近似（看图时注意）

- **字体 1（数值、标签、单位）是真实点阵**，所以数字能直接看清；
  **字体 2 / 4（卡片标题 "CPU"/"GPU"、顶部标题、hero 大字）画成等宽棋盘格占位块**，
  只用于核对位置和尺寸，不代表真实字形。
  **例外：开机画面的品牌字"奶娃科技"是 8 位 alpha 位图**（内置字体没有汉字字形），
  固件里按 `drawPixel` 逐点混合 —— 所以预览图里那四个字是**真机上的真实效果**。
- 圆角、sprite 推送、文本基准（TL/TR_DATUM）都按真实库的行为实现；但 `fillRoundRect`
  的圆角像素与真实库可能差 1px，属正常。
- `String` 是简化的 `std::string` 实现，浮点格式化用 `snprintf("%.*f")`，与 Arduino
  `String(float, dec)` 一致。
- 仿真里 `bleConnected` 始终为 false（没有蓝牙），所以标题栏胶囊固定显示红色的 OFFLINE。
- **配色不写死在脚本里**：卡片底色由 `simCardBg()` 从画面采样（取卡片内部一点），
  所以改 `screen_draw.cpp` 的配色不会让断言失效；只有行标签色（`C_LABEL 0xBDF7`）
  在 `simLabelIntact()` 里是写死的，改它要同步改那边。

## 改 UI 时的用法

改完 `screen_draw.cpp` 后重跑第 2 步，重点看两块：

1. `report.txt` 里的断言（任何 `!!` 开头的行都是失败）：
   - 标签逐像素校验必须"全部完好"
   - 数值右边界不能超过槽位；概览小图 `x26..311 y158..199`；趋势页大图 `x26..311 y42..153`
   - `fillScreen 调用次数=1`、稳态帧"直接画到屏幕的文字次数=0"（频闪红线）
   - 断线帧必须有 NO SIGNAL 且无曲线残留
2. 字符化画面（`=== P1/P2/P3 ... ===`）或直接用图片查看器打开 PNG/BMP。

## 加页面 / 改数值时的注意点

- 页面函数只负责"动态内容"，静态框架（卡片框、标签、图例、刻度）写在该页的
  `drawStaticXxx()` 里，由 `drawPageStatic()` 分发；切页时框架先整体重画。
- **切页不要用 `fillScreen`**：各页的实心卡片会互相覆盖，清屏只会带来整屏闪光。
  新页面如果有"旧页面画过、新页面盖不到"的区域（缝隙、圆角缺口），
  要在自己的静态函数里用 `clearCorners()` / `clearBeforeXxx()` 补清，
  否则会留下上一页的一条线（仿真的残留断言能抓到）。
- 每页的动态刷新必须带上 `c.full` 条件（切页/首帧/链路状态翻转时强制重画），
  否则切回该页会看到上一页或上一状态的残留。
- **任何会变的文字都要走 `regionBegin/regionEnd`**（画进离屏画布再整块推送），
  直接画到屏幕就是"先擦后画"的微闪，仿真里"稳态帧直接画到屏幕的文字次数"必须为 0。
- 字符串长度会变的文本（峰值、均值、窗口时长、采样率）必须整块擦+画，
  因为 `drawString` 只画字形不擦底（踩过坑：见 `updPeaks`）。
- 标签列宽度是硬约束：底栏/页脚的标签列只有 30px，超过 4 个字符会伸进数值槽被擦掉
  （踩过坑：页脚 `SAMPLE` → 改 `RATE`；系统卡片 `UPTIME` 用了 42px 专用列宽）。
- 新增布局常量后补一条 `static_assert`，越界会直接编译失败
  （踩过坑：系统卡片内容溢出 2px、波形区多出 4px 盖住卡片右边框）。
- **开机画面的版面数字在 `preview_main.cpp` 第 0 段里是手抄的**（`bTitleY=54` /
  `bTrackY=146` / `bStatusY=162` / `bHintY=188` / `bDevY=212`，来自 `screen_draw.cpp`
  的 `BOOT_*` 宏）。改那边要同步改这里 —— 和 `detectPage()` 里那几个手抄的
  `MET_BAR_X/MID_Y` 是同一个约定。
- 品牌字要改字/换字体：改 `tools/make_logo.py` 的 `TEXT` / `FONT_CANDIDATES`，
  跑一次重新生成 `PC_Monitor/boot_logo.h`（需要 Pillow，只是开发期工具）。

## 别用 PowerShell 碰源文件

`Get-Content -Raw` + `Set-Content` 的往返会把 UTF-8 中文注释变成乱码，
并且可能吃掉换行把下一行代码并进注释里（本项目已经踩过两次）。
改源码只用编辑工具，PowerShell 只用来跑命令和转图。
