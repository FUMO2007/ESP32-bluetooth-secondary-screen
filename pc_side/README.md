# pc_side —— PC 端（v2 二进制协议版）

这个目录是**工作区内的开发副本**，实际运行时用的是 `D:\PC_Monitor\PC端\` 下的文件。
2026-10-11 已经部署过一次（原文件备份为 `pc_monitor_libre.py.bak_v2_20261011_135701`）。

## 文件

| 文件 | 说明 |
| --- | --- |
| `pc_monitor_v2.py` | 新版采集 + BLE 发送（部署时改名为 `pc_monitor_libre.py`） |
| `proto_v2.py` | 协议实现（定标打包 / CRC8），**必须和主脚本放在同一目录** |
| `test_proto.py` | 协议自检 + **生成** C++ 侧的金标向量（`tools/ui_preview/golden_vectors.h`） |
| `test_pc_side.py` | 无硬件自检：型号名缩短、采集容错、打包往返、CSV 兼容、名称包 |
| `test_out.txt` | 最近一次 `test_pc_side.py` 的输出 |

## 怎么跑测试（都不需要硬件）

**推荐直接跑仓库根目录的一键脚本**，它会把下面这些全部跑一遍并检查每一步的退出码：

```powershell
.\build.cmd              # 全部（含 UI 仿真）—— 推荐入口，见下面的"注意"
.\build.cmd -Quick       # 跳过最慢的 UI 仿真

# 直接用 build.ps1 也行（等价，但可能被拦，见"注意"）：
powershell -NoProfile -File build.ps1          # 全部（含 UI 仿真）
powershell -NoProfile -File build.ps1 -Quick   # 跳过最慢的 UI 仿真
```

> **注意（本机实测）：** 直接跑 `powershell -File build.ps1` 可能被拦下来，报
> `The file ... is not digitally signed`。这不是脚本坏了：本机有活动的代码完整性
> 策略，**新建或被编辑过的 .ps1 会被拒绝执行** —— 一直没动过的 `deploy.ps1` 能跑，
> 但它的字节副本、以及改过一次的 `build.ps1` 都会被拦，与脚本内容无关。
> `-ExecutionPolicy Bypass` 只对本次进程跳过这层检查（不改机器设置），
> `build.cmd` 已经带上了这个开关。详细说明见 `build.ps1` 头部。

想单独跑某一步时：

```powershell
# 1) PC 侧逻辑（用 PC 端自带的 venv，-B 表示不写 __pycache__）
& 'D:\PC_Monitor\PC端\venv\Scripts\python.exe' -B pc_side\test_pc_side.py

# 2) 生成金标向量 + 协议自检
#    会写出 tools/ui_preview/golden_vectors.h —— **不要手工编辑那个文件**
python -B pc_side\test_proto.py

# 2b) 只校验头文件是不是最新的（不改文件，适合 CI / 提交前）
python -B pc_side\test_proto.py --check

# 3) C++ 侧解码同一向量（跨语言一致性）
g++ -std=gnu++11 -I tools/ui_preview -I PC_Monitor tools\ui_preview\proto_test.cpp PC_Monitor\proto.cpp `
    -o tools\ui_preview\proto_test.exe
tools\ui_preview\proto_test.exe
```

> 金标向量以前是"人把第 2 步打印的 HEX 复制粘贴进 `proto_test.cpp`"。那样
> **改 `proto_v2.py` 的 `FIELD_SCALE` 不会被任何测试发现**（C++ 侧还在用旧的
> 冻结向量）。现在向量由第 2 步生成、C++ 侧只负责解码比对，两端漂移必然暴露。

## 相对旧版（pc_monitor_libre.py）改了什么

1. 通信改二进制定标编码：31 字段 66 字节一包（旧 CSV 要 90 字节且加不了字段）；
   带序号 + CRC8，收包端能拒绝损坏/截断的包。
2. 新传感器：CPU 电压、最忙核心负载、GPU 热点、显存温度、GPU 核心频率、显存控制器负载、机箱风扇。
3. 新派生数据：磁盘读/写 MB/s、网络上/下行 MB/s、系统盘占用、进程数、运行时长。
4. 网络延迟探测：TCP 连接 1.1.1.1:53 的耗时 + 最近 10 次丢包率（不需要管理员）。
5. 真帧率：**只报实测值，绝不估算**（见下）。两个来源：PresentMon（随包分发）
   和 RTSS 共享内存（用户自己装了就用）。
6. 型号名低频单独发包，屏幕用它替换卡片副标题。
7. 风扇/温度的后备查找合并进主循环，去掉了每轮 3 次多余的硬件 `Update()`。

## 真帧率（FPS / TIME）

**屏幕上那个数字只有两种可能：实测，或者 `--`。** 没有第三种。

以前这里有一条"估算法"兜底：`fps = 60 * (1 - gpu_load/100 * 0.4)`。已删除，因为：

1. 它是编的 —— 跟真实帧率没有任何关系，算出来恒定落在 **36~60**，还随负载平滑
   变化，屏幕上看就是个非常可信的假读数。
2. 它连"参考价值"都没有 —— 该式是 `gpu_load` 的仿射函数（`fps = 60 - 0.24L`），
   而负载本来就显示在同一块屏上；给定负载这个"帧率"完全确定，**信息量为零**。

一块监控屏只要有一个数字是假的，其它数字也就没人信了。所以宁可显示 `--`。

### 两个来源

| | **PresentMon**（推荐） | RTSS |
| --- | --- | --- |
| 许可 | MIT，**可随程序分发** | 专有 freeware，不能再分发 |
| 形态 | 单文件便携 exe（~1 MB） | 需要安装（注入 DLL + 内核驱动 + 常驻） |
| 机制 | ETW 事件，**不注入** | DLL 注入（Detours） |
| 反作弊 | 无冲突 | 会被拦/踢 |
| 其他电脑能用 | ✅ 打包进去就行 | ❌ 要用户自己装 |

在 ini 里用 `FPS_SOURCE = auto | presentmon | rtss | none` 选择。

### PresentMon 的两个硬性前提

1. **必须提权。** 实测非管理员直接失败（exit code 6）：

   ```
   error: failed to start trace session: access denied.
   PresentMon requires either administrative privileges or to be run by a user in the
   "Performance Log Users" user group.
   ```

   所以 `main()` 里只要启用了 PresentMon 就要求提权（不能只在"读不到 CPU 温度"
   时才提 —— Intel 平台普通权限能读温度，那样 PresentMon 永远起不来）。
   替代方案：把自己加进组（`compmgmt.msc` → 本地用户和组 → 组 →
   `Performance Log Users`，加完要重新登录）。

2. **要有正在渲染的 3D 程序。** 回到桌面就没有帧率可测了，那时显示 `--` 是
   **正确行为**，不是坏了。

### 排查"屏幕显示 --"

`tools/probe_fps.py` 会把每一层都打出来（找没找到二进制、有没有管理员权限、
有没有帧、RTSS 在不在），不用盯着屏幕猜：

```powershell
# 请用管理员终端跑，否则结果没有参考价值
& 'D:\PC_Monitor\PC端\venv\Scripts\python.exe' -B tools\probe_fps.py --raw 8
```

### 选哪个进程的帧率（这条规则被真机数据改过两次）

一次 15 秒的实测采样里，同时 present 的进程有：

```
dwm.exe(5428)        71.6fps    桌面合成器
Endfield.exe(52352)  64.1fps    真正的游戏（也是前台）
wallpaper64.exe      29.8fps    壁纸引擎
HYPHelper.exe        357.3fps   游戏自带的无窗口渲染辅助
WindowsTerminal.exe  3.4fps     正在打字的终端
```

第一版规则是"**前台优先，否则取帧率最高**"，结果屏幕上在 **3 / 64 / 357** 之间乱跳：

- 你盯着监控画面时，前台恰好是**终端**（只在文字变化时 present）→ 报 3fps
- 前台落空时取"最高" → 抓到无窗口 helper 的 357fps

现在改用**结构性判据**：

> 只在**拥有可见、未最小化的、不小于 160×160 顶层窗口**的进程里选，
> 取其中**帧率最高**的；帧率打平时才用前台 pid 决胜。另外按名字排除
> `dwm.exe` / `explorer.exe` 这类"有窗口、也一直在 present，但永远不是游戏"的外壳。

这样一次排掉：无窗口 helper（没有窗口）、合成器（按名字）、终端（3fps 竞争不过游戏）。

`probe_fps.py` 的 `PresentMon : ...` 那行会**逐进程标注被选/被跳过的原因**
（`<-PICK` / `[skip:no-visible-window]` / `[skip:shell]`），选错了能一眼看出来。

### 顺带的能力

拿到的是**逐帧**数据，所以除了平均帧率还能算 1% low（描述卡顿的指标，平均帧率
对偶发尖峰完全无感）。目前协议里没有对应字段，只用在日志和探针里。

注意：`frame_ms` 与 `1000/fps` 在数学上是**相等**的（都是平均帧间隔），所以换用
PresentMon 的收益不在于"平均值更准"，而在于逐帧数据本身。

## 需要注意

- `Config.PROTOCOL`（ini 里的 `PROTOCOL`）可切回旧文本协议（只 14 个字段）用于排查。
- 改协议时**两端一起改**：`PROTOCOL.md` → 固件 `proto.cpp` 的 `SCALE` →
  这里 `proto_v2.py` 的 `FIELD_KEYS`/`FIELD_SCALE`，然后跑 `build.ps1`。
  只改一端会被第 1 步（`--check` 报头文件过期）或第 5 步（逐字段 raw 不一致）抓住。
- 运行时会写一份滚动日志 `pc_monitor.log`（打包后是 exe 旁边）。
  以前日志是 `CRITICAL` + `NullHandler` 且 stdout 被扔进 devnull，
  出错时一点痕迹都没有；现在启动失败、扫描不到设备、连不上、连不上后自动退出、
  ini 写错、传感器读不到，都会留痕，致命错误还会弹一个对话框。
- 打包用仓库根目录的 `pc_monitor_libre.spec`。它会自动把 `dlls/` 和
  `vendor/PresentMon-2.6.0-x64.exe` 一起打进去（找不到 PresentMon 只是丢帧率
  功能，不会让打包失败）。**注意 onefile 的解压目录会被删**，所以程序启动时
  会把 PresentMon 复制到 `%LOCALAPPDATA%\PCMonitor\` 再运行，不会出现
  "运行中的 exe 被删掉"。
