# vendor/ —— 随包分发的第三方可执行文件

## PresentMon-2.6.0-x64.exe

| | |
|---|---|
| 用途 | 真帧率的**唯一**可靠来源（ETW 事件，不注入进程，不与反作弊冲突） |
| 版本 | 2.6.0（x64 单文件便携版） |
| 来源 | https://github.com/GameTechDev/PresentMon/releases |
| 许可 | MIT —— **允许随程序分发**，但必须保留版权与许可声明（见下方"许可合规"） |
| SHA256 | `b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af` |

这个 SHA256 在两处被硬校验：

- `build.ps1` 第 3 步 —— 与官方 release digest 不一致就**直接让构建失败**（防止打进被掉包的文件）；
- `pc_monitor_libre.spec` —— 打包时打印校验结果（不一致只告警，因为允许用户有意换成别的版本）。

运行时还会把它从 onefile 解包目录复制到 `%LOCALAPPDATA%\PCMonitor\` 再启动
（`sys._MEIPASS` 是临时目录、父进程一退出就被删，而 PresentMon 是独立子进程、可能
比我们活得久）；复制后会**按哈希**比对随包那份，不一致就不执行它。

### 没有它会怎样

不会让构建或打包失败，只是丢掉帧率功能：屏幕上 `FPS / TIME / LOW` 显示 `--`。
这是刻意的 —— 别人从源码构建时不该被迫先去下一个第三方二进制。

### 为什么不用 RTSS

RTSS（RivaTuner）也能给帧率，但它是专有 freeware、**不允许再分发**，而且要装注入式
DLL + 内核驱动并常驻，还会被反作弊拦。所以仓库里只放 PresentMon；RTSS 作为可选来源
由用户自己安装（ini 里 `RTSS_ENABLE = 1`）。

## 许可合规

MIT 许可要求分发时保留版权声明与许可全文。上游的许可文本随 PresentMon 的 release
一起提供（`LICENSE`/`PresentMon/LICENSE`）。如果要公开分发打包好的 exe，
请把那份许可文本一并附上（例如放进 `vendor/LICENSE-PresentMon.txt` 并在界面的
"关于"或 README 里指明）。

## 不在这个目录里的第三方组件

- **LibreHardwareMonitorLib.dll 等**（MPL-2.0）：托管 DLL 集合，体积大且升级频繁，
  以 `dlls/` 的形式由使用者自行取得，不入库（见根 README 的"你需要准备"）。
- **TFT_eSPI**（MIT）：固件依赖，作为整库拷贝放在 `libraries/TFT_eSPI/`。
