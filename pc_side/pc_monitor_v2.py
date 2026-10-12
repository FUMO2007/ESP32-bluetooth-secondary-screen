# pc_monitor_v2.py
# ---------------------------------------------------------------------------
#  相对 pc_monitor_libre.py 的改动（配合固件 v2 二进制协议，见项目根 PROTOCOL.md）：
#   1) 通信改二进制定标编码：31 个字段 66 字节一包（旧 CSV 文本要 90 字节且加不了字段）
#   2) 新增传感器：CPU 电压 / 最忙核心 / GPU 热点 / 显存温度 / GPU 频率 / 显存控制器负载 / 机箱风扇
#   3) 新增 psutil 派生数据：磁盘读写 MB/s、网络上下行 MB/s、系统盘占用、进程数、运行时长
#   4) 新增网络延迟探测（TCP 连接耗时 + 丢包率，不需要管理员）
#   5) 新增 RTSS 共享内存真帧率（RivaTuner/MSI Afterburner 在跑时可用）；两个来源
#      都拿不到就是 None -> 屏幕显示 --（**没有**估算法兜底，见 pc_side/README.md）
#   6) 型号名低频单独发包，屏幕用它替换卡片副标题
#   7) 风扇/温度的后备查找合并进主循环，去掉了原来每轮 3 次多余的硬件 Update
# ---------------------------------------------------------------------------
import os
import re
import sys
import csv
import time
import asyncio
import psutil
import logging
import logging.handlers
import ctypes
import ctypes.wintypes
import subprocess
import threading
import hashlib
import collections
from typing import Optional, Tuple, Dict, Any, List

# ===== 打包后重定向输出到 null（完全静默）=====
if getattr(sys, 'frozen', False):
    try:
        sys.stdout = open(os.devnull, 'w')
        sys.stderr = open(os.devnull, 'w')
    except:
        pass

# ===== 立即隐藏控制台窗口（启动时执行）=====
# 设 PCMON_NO_HIDE=1 可跳过（离线单测时用，避免把测试终端一起藏掉）
if sys.platform == 'win32' and os.environ.get('PCMON_NO_HIDE') != '1':
    try:
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        user32 = ctypes.WinDLL('user32', use_last_error=True)
        console_window = kernel32.GetConsoleWindow()
        if console_window:
            user32.ShowWindow(console_window, 0)  # 0 = SW_HIDE
            kernel32.FreeConsole()
    except:
        pass


# ===== 自动提权 =====
# AMD CPU 的 Tctl/Tdie 温度和 Package/SMU 功耗传感器必须通过底层驱动读取，
# 需要管理员权限；否则读数恒为 0。启动时若非管理员，自动以管理员身份重启。
def ensure_admin() -> bool:
    """若当前不是管理员，尝试以管理员权限重启自身。
    返回 True 表示已成功启动提权实例（本进程应立即退出）。"""
    if sys.platform != 'win32':
        return False
    try:
        if ctypes.windll.shell32.IsUserAnAdmin():
            return False
        params = None
        if not getattr(sys, 'frozen', False):
            params = '"{}"'.format(os.path.abspath(sys.argv[0]))
        ret = ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable, params, None, 1)
        # 返回值 > 32 表示启动成功；用户取消 UAC 则返回 5，继续以普通权限运行
        return ret > 32
    except Exception:
        return False


# ===== 设置 DLL 路径（支持打包后运行）=====
def get_base_path():
    """获取程序运行基础路径"""
    if getattr(sys, 'frozen', False):
        return sys._MEIPASS
    else:
        return os.path.dirname(os.path.abspath(__file__))


BASE_PATH = get_base_path()
dlls_path = os.path.join(BASE_PATH, 'dlls')


# ===== 可选配置文件（放在 exe / 脚本旁边，改参数不用重新打包）=====
def get_app_dir():
    """程序所在目录：打包后是 exe 所在目录，不是解包临时目录 _MEIxxxx"""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


_INI_PROBLEMS = []      # logger 建好之前攒下的配置问题


def _ini_problem(msg: str):
    """报告一个配置问题。

    logger 的建立顺序有讲究：_load_ini() 跑在它前面，所以那时候只能先攒着；
    而 Config 类体里的 _ini_int/_ini_float 求值时 logger 已经就绪，可以直接记。
    （一开始我只做了"攒着"那一半，结果 Config 里的非法的值根本没报出来 ——
     是实测才发现的。）
    """
    lg = globals().get('log')
    if lg is not None:
        lg.warning('配置：%s', msg)
    else:
        _INI_PROBLEMS.append(msg)


class _IniValues(dict):
    """ini 的键值表。**访问过的键都记下来**，好在本文件后面报告"写了却没生效的键"。

    动机：键名拼错（`USRE_NAME=…`）以前是完全静默的 —— 用户以为改生效了，
    程序照旧用默认值，屏幕上一点变化都没有，只能靠猜。这属于本项目
    "出问题必须留痕"的原则。
    """

    def __init__(self):
        super().__init__()
        self.used = set()

    def get(self, key, default=None):
        self.used.add(key)
        return super().get(key, default)


def _load_ini(path=None):
    """读一个 ini 文件（默认是同目录下的 pc_monitor.ini，可选）。
    格式：一行一条 key=value，# 开头是注释。文件不存在是正常情况（全部用默认值）。

    path 参数是给测试用的：这样"带 BOM 的 ini"这类情形不用真的往程序目录写文件。
    """
    cfg = _IniValues()
    if path is None:
        path = os.path.join(get_app_dir(), 'pc_monitor.ini')
    if not os.path.exists(path):
        return cfg
    try:
        # encoding='utf-8-sig'：记事本"UTF-8"另存会把 BOM 写进文件头，
        # 用 'utf-8' 读的话第一个键会变成 '\ufeffPING_HOST' —— 它不等于 'PING_HOST'，
        # 于是那一条**静默失效**（只有第一条会中招，最难发现）。utf-8-sig 会自动吃掉 BOM。
        # 顺带：读到 GBK 文件会抛异常走下面的分支，而不是解码成乱码键名。
        with open(path, 'r', encoding='utf-8-sig') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#') or '=' not in line:
                    continue
                k, v = line.split('=', 1)
                cfg[k.strip().upper()] = v.strip()
    except Exception as e:
        # 以前这里是裸 except: pass —— ini 编码写错（比如存成 GBK）会**静默**
        # 退回全部默认值，用户改了半天以为没生效，其实一个字都没读到。
        _ini_problem('读取 %s 失败（%s: %s），本次全部使用默认值'
                     % (path, type(e).__name__, e))
        return cfg
    return cfg


_INI = _load_ini()


def _ini_int(key, default):
    _INI.used.add(key)
    if key not in _INI:
        return default
    try:
        return int(float(_INI[key]))
    except Exception:
        _ini_problem('%s = %r 不是数字，改用默认值 %r' % (key, _INI[key], default))
        return default


def _ini_float(key, default):
    _INI.used.add(key)
    if key not in _INI:
        return default
    try:
        return float(_INI[key])
    except Exception:
        _ini_problem('%s = %r 不是数字，改用默认值 %r' % (key, _INI[key], default))
        return default


def _ini_choice(key, default, allowed):
    """取一个枚举值（大小写不敏感）。不在 allowed 里就报警并退回默认值。

    以前这种值是直接 .strip().lower() 用的：`FPS_SOURCE = presentmno` 会被
    静默当成"不是 auto / presentmon / rtss / none"，各处的 if 全不匹配，
    表现是"帧率永远是 --"，而日志里一个字都没有。
    """
    _INI.used.add(key)
    raw = _INI.get(key, default).strip().lower()
    if raw not in allowed:
        _ini_problem('%s = %r 不是 %s 之一，改用默认值 %r'
                     % (key, _INI.get(key), '/'.join(allowed), default))
        return default
    return raw


def _report_unknown_ini_keys():
    """把"ini 里写了、但本程序从没读过"的键报出来（拼错键名就靠它发现）。"""
    unknown = sorted(set(_INI) - _INI.used)
    if unknown:
        _ini_problem('ini 里这些键没被认出来（键名拼错了？本程序不会读它们）：%s'
                     % '、'.join(unknown))


# 将 dlls 文件夹添加到系统路径
# 用 .get('PATH', '')：极少数环境（服务/精简容器）里根本没有 PATH 变量，
# 直接下标会抛 KeyError，而这个 KeyError 发生在导入期，报出来完全看不出是 PATH 的事。
_PATH_ENV = os.environ.get('PATH', '')
if os.path.exists(dlls_path) and dlls_path not in _PATH_ENV:
    os.environ['PATH'] = dlls_path + os.pathsep + _PATH_ENV

# ===== 日志 =====
# 以前这里是 logging.CRITICAL + NullHandler，再加上打包后 stdout/stderr 被重定向到
# devnull —— 结果"DLL 缺失 / clr 导入失败 / 扫描不到设备 / 连不上"全都没留下任何痕迹，
# 程序安静地什么都不做，10 分钟后按 AUTO_EXIT_MINUTES 自己退出，用户完全不知道为什么。
# 现在始终写一份滚动日志到 exe 旁边；打包后控制台不可见，致命错误再弹一个对话框。
LOG_PATH = os.path.join(get_app_dir(), 'pc_monitor.log')


def _setup_logging() -> logging.Logger:
    lg = logging.getLogger('pcmon')
    lg.setLevel(logging.DEBUG)
    lg.propagate = False        # 不冒泡到 root，免得第三方库把日志搅乱
    fmt = logging.Formatter('%(asctime)s %(levelname)-7s %(message)s',
                            '%Y-%m-%d %H:%M:%S')
    try:
        fh = logging.handlers.RotatingFileHandler(
            LOG_PATH, maxBytes=512 * 1024, backupCount=2, encoding='utf-8')
        fh.setFormatter(fmt)
        lg.addHandler(fh)
    except Exception:
        pass                    # 日志写不了不能拖垮主程序
    if not getattr(sys, 'frozen', False):
        try:                    # 直接跑 .py 时同时打到终端，方便排查
            sh = logging.StreamHandler()
            sh.setFormatter(fmt)
            lg.addHandler(sh)
        except Exception:
            pass
    return lg


log = _setup_logging()

# _load_ini / _ini_* 在 logger 建好之前就跑，攒下来的问题在这里统一报告
for _p in _INI_PROBLEMS:
    log.warning('配置：%s', _p)
_ini_path = os.path.join(get_app_dir(), 'pc_monitor.ini')
log.debug('配置文件: %s（%s）', _ini_path,
          '已读取' if os.path.exists(_ini_path) else '不存在，使用默认值')


def _fatal(msg: str):
    """记录致命错误。打包后控制台是隐藏的，只写日志用户看不到，所以再弹个框。"""
    log.error(msg)
    if sys.platform == 'win32' and getattr(sys, 'frozen', False):
        try:
            ctypes.windll.user32.MessageBoxW(
                None, msg, 'PC Monitor', 0x10 | 0x40000)   # MB_ICONERROR | MB_TOPMOST
        except Exception:
            pass

# ===== 导入其他库 =====
from bleak import BleakScanner, BleakClient, BleakError


def _ini_ranged(key, default, lo, hi, integer=False):
    """取一个带范围的数值：超范围就报警并夹到边界，不是静默接受。

    为什么非夹不可（都是"改一个数字"就能造成的难查故障）：
      COLLECT_INTERVAL = 0     -> 主循环满速忙等，把 CPU 吃满，而这台机器自己
                                  就在监控 CPU 占用，屏幕上会看到"自己把自己顶满"
      PING_WINDOW = 0          -> 丢包率的分母是 0，LOSS 恒显示 0.0
      AUTO_EXIT_MINUTES = 0    -> "连不上就退出"立刻触发，程序一闪而过
    """
    raw = _ini_int(key, default) if integer else _ini_float(key, default)
    if raw < lo or raw > hi:
        fixed = min(max(raw, lo), hi)
        _ini_problem('%s = %r 超出允许范围 [%s, %s]，改用 %r'
                     % (key, raw, lo, hi, fixed))
        raw = fixed
    return int(raw) if integer else float(raw)


# ===== 配置 =====
class Config:
    DEVICE_NAME = _INI.get('DEVICE_NAME', "PC-MONITOR-ST7789").strip() or "PC-MONITOR-ST7789"
    CHAR_WRITE_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
    LIBRE_DLL_PATH = os.path.join(dlls_path, "LibreHardwareMonitorLib.dll")
    COLLECT_INTERVAL = _ini_ranged('COLLECT_INTERVAL', 1.0, 0.1, 60.0)  # 采集间隔（秒）
    # 下面这些以前是硬编码的，但它们恰恰是用户最想调的几个 ——
    # 尤其是 AUTO_EXIT_MINUTES（程序会自己关掉）和 MAX_RECONNECT_ATTEMPTS
    # （刷几次固件程序就永久退出）。ini 存在的意义就是"改参数不用重新打包"。
    MAX_RECONNECT_ATTEMPTS = _ini_ranged('MAX_RECONNECT_ATTEMPTS', 3, 0, 100, integer=True)
    SCAN_TIMEOUT = _ini_ranged('SCAN_TIMEOUT', 5.0, 1.0, 120.0)
    CONNECT_TIMEOUT = _ini_ranged('CONNECT_TIMEOUT', 10.0, 1.0, 300.0)
    AUTO_EXIT_MINUTES = _ini_ranged('AUTO_EXIT_MINUTES', 10, 1, 1440, integer=True)

    # ---- 协议 ----
    PROTOCOL = _ini_choice('PROTOCOL', 'binary', ('binary', 'csv'))
    NAMES_RESEND_SEC = _ini_ranged('NAMES_RESEND_SEC', 60.0, 5.0, 3600.0)

    # ---- 真帧率 ----
    # 'auto'       = 先试 PresentMon，再试 RTSS（装了哪个就用哪个，都不用装）
    # 'presentmon' = 只用 PresentMon
    # 'rtss'       = 只用 RTSS
    # 'none'       = 不测帧率，屏幕永远是 "--"
    # 两个来源都拿不到数据时一律上报"无效"（0xFFFF -> 屏幕显示 --），**绝不猜一个数**。
    FPS_SOURCE = _ini_choice('FPS_SOURCE', 'auto', ('auto', 'presentmon', 'rtss', 'none'))
    PRESENTMON_PATH = _INI.get('PRESENTMON_PATH', '').strip()   # 留空 = 自动查找
    RTSS_ENABLE = _ini_int('RTSS_ENABLE', 1) != 0

    # ---- 网络延迟探测 ----
    # 默认用阿里 DNS：1.1.1.1 在国内 TCP 往返实测 233ms，223.5.5.5 只要 39ms，
    # 而 PING 超过 100ms 会变黄 —— 挂 Cloudflare 就会一直黄着，看着像坏了。
    # 想换目标不用重新打包：改 exe 旁边的 pc_monitor.ini 里的 PING_HOST / PING_PORT。
    PING_HOST = _INI.get('PING_HOST', "223.5.5.5")
    PING_PORT = _ini_ranged('PING_PORT', 53, 1, 65535, integer=True)
    PING_INTERVAL = _ini_ranged('PING_INTERVAL', 5.0, 0.2, 600.0)
    PING_WINDOW = _ini_ranged('PING_WINDOW', 10, 1, 1000, integer=True)


# 放在 Config 之后：上面的类体已经把该读的键都读过了，这时才知道哪些键没人认领。
_report_unknown_ini_keys()


# ===== 协议实现（独立模块，便于离线单测；与固件 proto.cpp / PROTOCOL.md 一一对应）=====
from proto_v2 import (pack_binary, pack_names as _pack_names, crc8,
                      PROTO_FIELD_COUNT, PROTO_PKT_DATA, PROTO_PKT_NAMES,
                      PROTO_INVALID, FIELD_KEYS, FIELD_SCALE)


# 已经就"超量程"告警过的字段，避免每秒刷一条
_SATURATION_WARNED = set()


def _fit_field(key: str, value):
    """值超出该字段的 uint16 量程时上报"无效"，而不是让它悄悄撞上限。

    协议是定标 uint16（上限 65534，0xFFFF 留给哨兵），超量程以前是**静默夹住**的：
    屏幕上是"一个看起来很正常的具体数字"，但它是错的 ——
      uptime_min   ×1     -> 65534 分钟 = 45.5 天（连续开机两个多月后一直显示这个数）
      net_up/down  ×100   -> 655.34 MB/s（万兆网卡跑满就顶在这个数上不动）
      disk_read/write ×10 -> 6553.4 MB/s（PCIe 5.0 NVMe 顺序读能超过它）
    按本项目"宁可显示 -- 也不显示编出来的数"的原则，超量程一律报无效。
    """
    if value is None:
        return None
    limit = (PROTO_INVALID - 1) / FIELD_SCALE[FIELD_KEYS.index(key)]
    if value > limit:
        if key not in _SATURATION_WARNED:
            _SATURATION_WARNED.add(key)
            log.warning('%s = %.2f 超出协议量程（该字段上限 %.2f），将一直显示 --；'
                        '要显示这么大的值就得改协议缩放（两端一起改）',
                        key, value, limit)
        return None
    return value


def short_name(s: str) -> str:
    """把冗长的硬件名缩成能放进卡片副标题的型号（≤ 20 字符）"""
    if not s:
        return ""
    t = re.sub(r'\(R\)|\(TM\)|\(C\)', '', s, flags=re.I)
    t = re.sub(r'\b(CPU|Processor|Graphics|Adapter|Device)\b', '', t, flags=re.I).strip()
    # CPU：i5-12400 / Ultra 7 155H / Ryzen 5 5600X / Threadripper ...
    m = re.search(r'\b(Ultra\s+\d\s+\d{3}\w*|i[3579]-\d{4}\w*|Ryzen\s+\d\s+\d{4}\w*'
                  r'|Threadripper\s+\w+|EPYC\s+\w+)', t, re.I)
    if m:
        return m.group(1).strip()
    # GPU：RTX 4060 / 4070 Ti / 4070 Ti SUPER / 4070 SUPER / GTX 1660 Ti / RX 6800 XT / Arc A770
    # 备选顺序必须是"长的在前"：只写 (Ti|SUPER)? 的话 "RTX 4070 Ti SUPER" 匹配到
    # "RTX 4070 Ti" 就停了 —— 屏上认不出它是 SUPER 版，而 4070 Ti 和 4070 Ti SUPER
    # 是两张性能不同的卡。（这个错误行为以前还被写进了测试的期望值里。）
    m = re.search(r'\b(RTX\s?\d{4}\s?(?:Ti\s?SUPER|SUPER|Ti)?'
                  r'|GTX\s?\d{3,4}\s?(?:Ti\s?SUPER|SUPER|Ti)?'
                  r'|RX\s?\d{4}\s?(?:XTX|XT|GRE)?|Arc\s?[A-Z]\d{3})\b', t, re.I)
    if m:
        return m.group(1).strip()
    return t[:20].strip()


# ===== 派生速率（把累计字节数换算成 MB/s）=====
class RateMeter:
    def __init__(self, getter):
        self.getter = getter
        self.prev = None
        self.prev_t = None
        self.mbs = 0.0

    def sample(self) -> float:
        try:
            cur = self.getter()
            # 用 monotonic 而不是墙钟：这里算的是"两次采样之间过了多久"，
            # NTP 校时/跨夏令时会让这个差值变成负数或突然变大，速率随即变成一个
            # 假尖峰（或负数）。
            now = time.monotonic()
            if self.prev is not None and self.prev_t is not None and now > self.prev_t:
                self.mbs = max(0.0, (cur - self.prev) / (now - self.prev_t) / (1024.0 * 1024.0))
            self.prev = cur
            self.prev_t = now
        except Exception:
            pass
        return self.mbs


# ===== 网络延迟探测（异步，不阻塞采集）=====
class PingProbe:
    def __init__(self):
        self.ms: Optional[float] = None
        self.loss: Optional[float] = None
        self._samples: List[Optional[float]] = []

    async def run(self):
        while True:
            t0 = time.perf_counter()
            ms = None
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(Config.PING_HOST, Config.PING_PORT), timeout=1.0)
                ms = (time.perf_counter() - t0) * 1000.0
                writer.close()
                try:
                    await writer.wait_closed()
                except Exception:
                    pass
            except Exception:
                ms = None

            self._samples.append(ms)
            if len(self._samples) > Config.PING_WINDOW:
                self._samples.pop(0)
            good = [s for s in self._samples if s is not None]
            self.ms = (sum(good) / len(good)) if good else None
            self.loss = 100.0 * (len(self._samples) - len(good)) / max(1, len(self._samples))
            await asyncio.sleep(Config.PING_INTERVAL)


# ===== RTSS 真帧率（RivaTuner Statistics Server 共享内存）=====
class RTSSReader:
    """从 RTSSSharedMemoryV2 读真实帧率。

    只在检测到某进程的帧计数在增长时才算数：fps = Δ帧数 / Δt。
    任何一步不对（RTSS 没装/没跑/结构体版本不同）都返回 (None, None)，
    由调用方原样当成"读不到" -> 屏幕显示 --（不估算、不兜底）。
    本机没有 RTSS，这段是按 RTSS SDK 的
    RTSS_SHARED_MEMORY / RTSS_SHARED_MEMORY_APP_ENTRY 布局写的，属于防御式实现。
    """
    SIGNATURE = 0x53535452           # 'RTSS'
    FILE_MAP_READ = 0x0004
    MAX_PATH = 260                   # APP 条目里 szName 的长度
    # 条目内偏移：dwProcessID(0) szName(4) dwFlags(264) dwTime0(268) dwTime1(272) dwFrames(276)

    def __init__(self):
        self.available = False
        self._view = None
        self._k32 = None
        self._u32 = None
        self._prev: Dict[int, int] = {}
        self._prev_t: Optional[float] = None
        self._warned = False
        if sys.platform != 'win32':
            return
        try:
            self._k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            self._u32 = ctypes.WinDLL('user32', use_last_error=True)
            self._k32.OpenFileMappingW.restype = ctypes.c_void_p
            self._k32.MapViewOfFile.restype = ctypes.c_void_p
            handle = self._k32.OpenFileMappingW(self.FILE_MAP_READ, False, "RTSSSharedMemoryV2")
            if handle:
                self._view = self._k32.MapViewOfFile(handle, self.FILE_MAP_READ, 0, 0, 0)
                self.available = bool(self._view)
        except Exception:
            self.available = False

    def _foreground_pid(self) -> int:
        try:
            hwnd = self._u32.GetForegroundWindow()
            pid = ctypes.wintypes.DWORD(0)
            self._u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            return int(pid.value)
        except Exception:
            return 0

    def _u32_at(self, addr: int) -> int:
        return ctypes.c_uint32.from_address(addr).value

    def read(self) -> Tuple[Optional[float], Optional[float]]:
        """返回 (fps, frametime_ms)；不可用时 (None, None)"""
        if not self.available or not self._view:
            return None, None
        try:
            if self._u32_at(self._view) != self.SIGNATURE:
                return None, None
            entry_size = self._u32_at(self._view + 8)
            arr_off = self._u32_at(self._view + 12)
            arr_size = self._u32_at(self._view + 16)
            if not (16 <= entry_size <= 8192) or not (0 < arr_size <= 256):
                return None, None

            now = time.monotonic()          # 同理：这是时间差，不能用墙钟
            dt = (now - self._prev_t) if self._prev_t else 0.0
            fg_pid = self._foreground_pid()
            best_fps = None
            best_fore = False

            for i in range(arr_size):
                base = self._view + arr_off + i * entry_size
                pid = self._u32_at(base)
                if pid == 0:
                    continue
                frames = self._u32_at(base + 276)
                prev = self._prev.get(pid)
                self._prev[pid] = frames
                if prev is None or dt < 0.2:
                    continue
                delta = frames - prev
                if delta <= 0:
                    continue
                fps = delta / dt
                if not (0.5 < fps < 2000.0):
                    continue
                is_fore = (pid == fg_pid)
                if best_fps is None or (is_fore and not best_fore) or (is_fore == best_fore and fps > best_fps):
                    best_fps = fps
                    best_fore = is_fore

            self._prev_t = now
            if best_fps is None:
                return None, None
            return best_fps, (1000.0 / best_fps if best_fps > 0 else None)
        except Exception:
            if not self._warned:
                self._warned = True
                log.warning('RTSS 共享内存读取失败（帧率将交给其它来源或显示 --）')
            return None, None


# ============================================================================
#  PresentMon 真帧率（可随本程序一起分发）
# ----------------------------------------------------------------------------
#  为什么除 RTSS 之外还要支持它：
#    RTSS 是闭源专有 freeware，而且依赖 DLL 注入（RTSSHooks64.dll / Detours）
#    和内核驱动，安装要管理员、机器级安装 —— **不能再分发**，用户必须自己装。
#    PresentMon 是 Intel 开源的（MIT），单文件便携 exe，走 ETW 事件、不注入，
#    所以可以随本程序一起打包，换台电脑开箱即用，也不会有反作弊冲突。
#
#  前提：PresentMon 必须能起 ETW 会话 —— 需要管理员权限，或当前用户属于
#  "Performance Log Users" 组。否则它直接失败（实测 exit code 6）：
#      error: failed to start trace session: access denied.
#  main() 的提权判断已经把这一条算进去了。
# ============================================================================
PRESENTMON_VERSION = '2.6.0'
PRESENTMON_EXE = 'PresentMon-%s-x64.exe' % PRESENTMON_VERSION
# 官方 release 的 SHA256（GitHub release asset digest），用来校验随包的二进制
PRESENTMON_SHA256 = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'

PRESENTMON_WINDOW = 120      # 用最近多少帧估计**当前**帧率（60fps 下约 2 秒）

# 1% low 的统计窗口。**不能**复用上面那个 120 帧的短窗口：
# 120 帧的 1% 只有 1.2 帧，算出来就是"最差的单帧"，那是 min 不是 1% low。
# 要有意义至少要几百帧，这里按时间取 60 秒（60fps 下 3600 帧，1% = 36 帧）。
LOW_WINDOW_SEC = 60.0
LOW_MIN_FRAMES = 100        # 样本不足就不报（宁可 -- 也不给个没意义的数）


def _foreground_pid() -> int:
    """前台窗口的进程号：两个帧率来源都用它挑"用户正在玩的那个进程"。
    拿不到返回 0，此时退化成"取帧率最高的那个"。"""
    if sys.platform != 'win32':
        return 0
    try:
        u32 = ctypes.WinDLL('user32', use_last_error=True)
        hwnd = u32.GetForegroundWindow()
        pid = ctypes.wintypes.DWORD(0)
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return int(pid.value)
    except Exception:
        return 0


def _split_csv(line: str) -> List[str]:
    """按 CSV 规则拆一行（进程名理论上可能含逗号，所以用标准库而不是 split(',')）"""
    try:
        return next(csv.reader([line]))
    except Exception:
        return line.split(',')


def _visible_window_pids(min_side: int = 160) -> set:
    """拥有"像样的可见顶层窗口"的进程号集合。

    这是挑选"用户正在玩的那个进程"的**结构性判据**。

    为什么不能只靠"前台窗口"或"帧率最高"（这两个规则都被真机数据证伪了）：
      · 桌面合成器 dwm.exe 一直在 present，帧率常年 60~70
      · 游戏自带的辅助进程（实测某个 HYPHelper.exe）会以 **357fps** 空转 present，
        但它没有任何可见窗口
      · 壁纸引擎这类东西也在后台不停 present
      · **而终端/浏览器一旦抢到前台，就会把它的低帧率（实测 3fps）当成"你的帧率"**
        —— 你正盯着这个监控画面时，前台恰恰就是终端

    "拥有一个不小于 min_side x min_side 的可见、未最小化顶层窗口"能一次性排掉
    前三种（它们没有这种窗口），第二种规则让游戏稳定胜出（其他可见窗口的应用
    不会持续 present）。
    """
    if sys.platform != 'win32':
        return set()
    pids = set()
    try:
        u32 = ctypes.WinDLL('user32', use_last_error=True)

        class RECT(ctypes.Structure):
            _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                        ('right', ctypes.c_long), ('bottom', ctypes.c_long)]

        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                         ctypes.c_void_p)

        def cb(hwnd, _lparam):
            try:
                if not u32.IsWindowVisible(hwnd):
                    return True
                if u32.IsIconic(hwnd):          # 最小化的不算"在看"
                    return True
                r = RECT()
                if not u32.GetWindowRect(hwnd, ctypes.byref(r)):
                    return True
                if (r.right - r.left) < min_side or (r.bottom - r.top) < min_side:
                    return True                 # 托盘/小工具类窗口
                pid = ctypes.wintypes.DWORD(0)
                u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value:
                    pids.add(int(pid.value))
            except Exception:
                pass
            return True

        u32.EnumWindows(WNDENUMPROC(cb), 0)
    except Exception:
        return set()
    return pids


# 桌面合成器/外壳：它们有窗口、也一直在 present，但永远不是"你在玩的游戏"。
# 结构判据排不掉它们（窗口又大又可见），所以按名字排除。
# 好消息是这个列表不需要完整 —— 低帧率的漏网之鱼会在"取最高帧率"时自然落选，
# 这里只需要挡住那些"帧率高但不是游戏"的。
_WINDOW_MANAGER_NAMES = {'dwm.exe', 'explorer.exe'}


def _is_excluded_name(name: str) -> bool:
    return bool(name) and name.strip().lower() in _WINDOW_MANAGER_NAMES


class PresentMonCsv:
    """PresentMon console 的 CSV 流解析器。

    **纯逻辑：不启动进程、不碰系统，所以可以离线单测**（见 test_pc_side.py）。
    这是有意的 —— 本机没有游戏、也没法在这里提权跑 ETW，能自动验证的就只有这一层。

    按**列名**而不是列号取值：PresentMon 的默认列集一直在变（2.6.0 就新增了
    source ID / layer ID 等），按名字找才不会跟着版本升级而崩。

    每行是一帧，只关心两列：
        ProcessID           哪个进程画的
        MsBetweenPresents   这一帧距上一帧多少毫秒（**逐帧原始数据**）
    帧率 = 1000 / 最近 N 帧的平均帧间隔。

    拿到的是逐帧数据，所以除了平均值还能算 1% low 这类卡顿指标
    （平均帧率对偶发尖峰完全无感）—— 这是它相对"只数帧数"的主要价值。
    """

    def __init__(self, window: int = PRESENTMON_WINDOW):
        self.window = max(2, int(window))
        self._cols: Optional[Dict[str, int]] = None
        self._frames: Dict[int, List[float]] = {}     # pid -> 最近 N 个帧间隔(ms)
        # pid -> deque[(单调时钟, 帧间隔ms)]，用于算 1% low；按时间裁剪，
        # 所以窗口长度和帧率无关（30fps 和 300fps 都是同一个时间跨度）
        self._low: Dict[int, 'collections.deque'] = {}
        self._names: Dict[int, str] = {}              # pid -> 进程名（日志/诊断用）
        self._last_seen: Dict[int, int] = {}          # pid -> 更新序号（用于淘汰）
        self._tick = 0
        self.header_seen = False
        self.rows = 0                                 # 累计接受的帧数

    def feed(self, line: str) -> bool:
        """喂一行。返回 True 表示这行是一帧有效数据。

        表头之前的噪声行（PresentMon 会先打一段多行 warning）、'NA' 字段、
        console 统计行都会被安静跳过 —— 调用方不需要做任何预处理。
        """
        line = line.lstrip('\ufeff').strip()
        if not line:
            return False
        parts = _split_csv(line)

        if not self.header_seen:
            idx = {}
            for i, name in enumerate(parts):
                idx[name.strip()] = i
            if 'ProcessID' in idx and 'MsBetweenPresents' in idx:
                self._cols = idx
                self.header_seen = True
                log.info('PresentMon CSV 表头识别成功（%d 列）', len(parts))
            return False

        cols = self._cols
        if cols is None:
            return False
        if len(parts) <= max(cols['ProcessID'], cols['MsBetweenPresents']):
            return False
        try:
            pid = int(parts[cols['ProcessID']])
        except (ValueError, TypeError):
            return False                    # 警告行/统计行，不是数据
        try:
            ms = float(parts[cols['MsBetweenPresents']])
        except (ValueError, TypeError):
            return False                    # 'NA' —— 该进程的第一帧没有"上一帧"
        if not (0.0 < ms < 10000.0):
            return False

        buf = self._frames.get(pid)
        if buf is None:
            buf = self._frames[pid] = []
        buf.append(ms)
        if len(buf) > self.window:
            del buf[0:len(buf) - self.window]

        # 1% low 用的长窗口（按时间裁剪，与帧率无关）
        low = self._low.get(pid)
        if low is None:
            low = self._low[pid] = collections.deque()
        now = time.monotonic()
        low.append((now, ms))
        cutoff = now - LOW_WINDOW_SEC
        while low and low[0][0] < cutoff:
            low.popleft()

        j = cols.get('Application')
        if j is not None and j < len(parts):
            self._names[pid] = parts[j]

        self._tick += 1
        self._last_seen[pid] = self._tick
        self.rows += 1
        self._evict()
        return True

    def _evict(self, keep: int = 64):
        """丢弃最久没更新的进程，避免长时间运行后字典无限增长"""
        if len(self._frames) <= keep:
            return
        for pid, _ in sorted(self._last_seen.items(), key=lambda kv: kv[1])[:len(self._frames) - keep]:
            self._frames.pop(pid, None)
            self._low.pop(pid, None)
            self._last_seen.pop(pid, None)
            self._names.pop(pid, None)

    def _candidates(self, allow_pids=None) -> Dict[int, Tuple[float, float]]:
        """当前可作为"你在玩的游戏"的进程 -> (fps, 平均帧间隔ms)。

        allow_pids 为 None 表示不做窗口过滤（单测用）；实机上由
        PresentMonReader 传入 _visible_window_pids() 的结果。
        """
        out = {}
        for pid, buf in self._frames.items():
            if allow_pids is not None and pid not in allow_pids:
                continue
            if _is_excluded_name(self._names.get(pid, '')):
                continue
            if len(buf) < 2:
                continue
            avg = sum(buf) / len(buf)
            if not (0.5 < avg < 2000.0):
                continue
            out[pid] = (1000.0 / avg, avg)
        return out

    def pick(self, allow_pids=None,
             foreground_pid: int = 0) -> Tuple[Optional[int], Optional[float], Optional[float]]:
        """挑出"用户正在玩的那个进程"，返回 (pid, fps, avg_ms)。

        规则：候选里取**帧率最高**的，帧率打平时才用前台 pid 决胜。

        注意这里**不再"前台优先"**了 —— 那个规则在真机数据上被证伪：你盯着监控
        画面时前台恰好是终端（3fps），一按 Alt-Tab 又变成别的，屏幕上会疯狂跳
        3 / 64 / 357。改成"可见窗口 + 取最高"之后，游戏稳定胜出。
        （推理见 _visible_window_pids 的注释。）
        """
        cands = self._candidates(allow_pids)
        if not cands:
            return None, None, None
        best = max(cands, key=lambda p: (round(cands[p][0], 1), p == foreground_pid))
        fps, avg = cands[best]
        return best, fps, avg

    def read(self, allow_pids=None,
             foreground_pid: int = 0) -> Tuple[Optional[float], Optional[float]]:
        """返回 (fps, frametime_ms)；没有合格候选时 (None, None)。

        注意 frametime_ms 与 1000/fps 在数学上**是相等的**（都是平均帧间隔），
        所以换用 PresentMon 的收益不在于"平均值更准"，而在于拿到的是**逐帧原始
        数据**：可以进一步算 1% low 这类描述卡顿的指标（见 low_fps()）。
        """
        _pid, fps, avg = self.pick(allow_pids, foreground_pid)
        if fps is None:
            return None, None
        return fps, avg

    def low_fps(self, pct: float = 1.0, allow_pids=None,
                foreground_pid: int = 0) -> Optional[float]:
        """选中进程的 "1% low" 帧率：窗口内最慢的那 pct% 的帧的平均帧率。

        这是唯一能看出卡顿的指标 —— 平均帧率 60fps 的序列里插一帧 200ms 的
        尖峰，平均值几乎不动，但实际体验是明显一顿。实测出现过平均 57fps、
        1% low 只有 12fps 的情况。

        用的是 LOW_WINDOW_SEC 秒的**长窗口**，不是那个 120 帧的短窗口：
        120 帧的 1% 只有 1.2 帧，等于取最差单帧 —— 那是 min，不是 1% low。
        样本不足 LOW_MIN_FRAMES 时返回 None（宁可显示 -- 也不给个没意义的数）。
        """
        pid, _fps, _avg = self.pick(allow_pids, foreground_pid)
        if pid is None:
            return None
        buf = self._low.get(pid)
        if not buf or len(buf) < LOW_MIN_FRAMES:
            return None
        vals = sorted((ms for _t, ms in buf), reverse=True)   # 最慢的在前
        n = max(1, int(round(len(vals) * pct / 100.0)))
        avg = sum(vals[:n]) / n
        return (1000.0 / avg) if avg > 0 else None

    def describe(self, allow_pids=None, foreground_pid: int = 0) -> str:
        """一行诊断信息：列出每个进程，并注明它为什么被选/被跳过。

        这个"说明理由"的输出很重要 —— 进程选择规则是本模块最容易出错的地方
        （第一版就栽在"前台优先"上），必须能一眼看出是它选错了还是数据不对。
        """
        if not self.header_seen:
            return '还没识别到 CSV 表头'
        if not self._frames:
            return '表头已识别，但还没有任何帧（是不是没在跑 3D 程序？）'
        picked, _fps, _avg = self.pick(allow_pids, foreground_pid)
        items = sorted(self._frames.items(), key=lambda kv: -len(kv[1]))
        bits = []
        for pid, buf in items[:6]:
            name = self._names.get(pid, '?')
            avg = sum(buf) / len(buf) if buf else 0.0
            fps = (1000.0 / avg) if avg else 0.0
            if pid == picked:
                why = ' <-PICK'
            elif _is_excluded_name(name):
                why = ' [skip:shell]'
            elif allow_pids is not None and pid not in allow_pids:
                why = ' [skip:no-visible-window]'
            elif len(buf) < 2:
                why = ' [skip:too-few-samples]'
            else:
                why = ''
            bits.append('%s(%d) %.1ffps/%.2fms n=%d%s'
                        % (name, pid, fps, avg, len(buf), why))
        return ' | '.join(bits)


def _user_data_dir() -> str:
    """一个稳定可写的目录：%LOCALAPPDATA%\\PCMonitor"""
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    p = os.path.join(base, 'PCMonitor')
    try:
        os.makedirs(p, exist_ok=True)
    except Exception:
        pass
    return p


def _sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def find_presentmon_exe(configured: str = '') -> Optional[str]:
    """找 PresentMon console 可执行文件，返回绝对路径；找不到返回 None。

    按优先级：
      1. ini 里显式指定的 PRESENTMON_PATH
      2. 本程序旁边（打包后就是 exe 同目录，最适合"放一个 exe 就能用"）
      3. 打包时随包分发的副本（sys._MEIPASS）—— 会先复制到稳定目录再返回
      4. %LOCALAPPDATA%\\PCMonitor（上次复制出来的那份）
      5. 仓库里的 vendor\\（直接跑 .py 开发时用）
      6. PATH 上任意 PresentMon-*-x64.exe

    找不到就返回 None —— 此时帧率就是 "--"，**不做任何猜测**。
    """
    def ok(p):
        return bool(p) and os.path.isfile(p) and p.lower().endswith('.exe')

    if ok(configured):
        return os.path.abspath(configured)

    app_dir = get_app_dir()
    for cand in (os.path.join(app_dir, PRESENTMON_EXE),
                 os.path.join(app_dir, 'vendor', PRESENTMON_EXE)):
        if ok(cand):
            return os.path.abspath(cand)

    # onefile 的 sys._MEIPASS 是临时目录，父进程一退出就被删。PresentMon 是
    # 独立子进程、可能比我们活得久，所以先复制到稳定目录再启动，避免"运行中的
    # exe 被删掉"（顺带也躲开"从 %TEMP% 启动 exe"这种容易被杀软盯上的行为）。
    meipass = getattr(sys, '_MEIPASS', None)
    if meipass:
        src = os.path.join(meipass, PRESENTMON_EXE)
        if ok(src):
            dst = os.path.join(_user_data_dir(), PRESENTMON_EXE)
            try:
                # 判断"要不要重放"不能只看文件大小：同尺寸的损坏文件或被替换的文件
                # 照样会被拿去执行，而它是从 %LOCALAPPDATA%（用户可写目录）启动的。
                # 比照对象是**随包那份**，不是写死的官方常量 —— spec 明确允许用户
                # 有意换成别的版本，那种情况下不该拦。
                if not os.path.isfile(dst) or _sha256_file(dst) != _sha256_file(src):
                    import shutil
                    shutil.copy2(src, dst)
                    log.info('已把 PresentMon 释放到 %s', dst)
                if _sha256_file(dst) != _sha256_file(src):
                    log.error('PresentMon 副本与随包那份不一致（%s）—— 不执行它，'
                              '改用临时目录里的副本，帧率显示 --', dst)
                    return src
                # 随包那份自己是不是官方版本：只提示，不拦（见上）
                if _sha256_file(dst) != PRESENTMON_SHA256:
                    log.warning('PresentMon 的 SHA256 与随包记录的官方值不同（%s）——'
                                '如果是有意换的版本，忽略这条；否则建议重新下载',
                                _sha256_file(dst))
                return dst
            except Exception as e:
                log.warning('释放 PresentMon 失败（%s），改用临时目录里的副本', e)
                return src

    cached = os.path.join(_user_data_dir(), PRESENTMON_EXE)
    if ok(cached):
        return cached

    repo_vendor = os.path.join(get_app_dir(), '..', 'vendor', PRESENTMON_EXE)
    if ok(repo_vendor):
        return os.path.abspath(repo_vendor)

    p = shutil_which_presentmon()
    if p:
        return p
    return None


def shutil_which_presentmon() -> Optional[str]:
    """在 PATH 上找任意版本的 PresentMon（用户可能自己装了 / 用 winget 装了）"""
    import glob
    dirs = (os.environ.get('PATH') or '').split(os.pathsep)
    for d in dirs:
        if not d:
            continue
        try:
            for hit in glob.glob(os.path.join(d, 'PresentMon*-x64.exe')):
                return os.path.abspath(hit)
        except Exception:
            continue
    return None


# ---- Job Object 用的结构体（必须定义在模块级：ctypes 不允许类体里自引用）----
class _JOBOBJ_IO_COUNTERS(ctypes.Structure):
    _fields_ = [('ReadOperationCount', ctypes.c_ulonglong),
                ('WriteOperationCount', ctypes.c_ulonglong),
                ('OtherOperationCount', ctypes.c_ulonglong),
                ('ReadTransferCount', ctypes.c_ulonglong),
                ('WriteTransferCount', ctypes.c_ulonglong),
                ('OtherTransferCount', ctypes.c_ulonglong)]


class _JOBOBJ_BASIC_LIMIT(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64),
                ('PerJobUserTimeLimit', ctypes.c_int64),
                ('LimitFlags', ctypes.c_uint32),
                ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t),
                ('ActiveProcessLimit', ctypes.c_uint32),
                ('Affinity', ctypes.c_size_t),
                ('PriorityClass', ctypes.c_uint32),
                ('SchedulingClass', ctypes.c_uint32)]


class _JOBOBJ_EXTENDED_LIMIT(ctypes.Structure):
    _fields_ = [('BasicLimitInformation', _JOBOBJ_BASIC_LIMIT),
                ('IoInfo', _JOBOBJ_IO_COUNTERS),
                ('ProcessMemoryLimit', ctypes.c_size_t),
                ('JobMemoryLimit', ctypes.c_size_t),
                ('PeakProcessMemoryUsed', ctypes.c_size_t),
                ('PeakJobMemoryUsed', ctypes.c_size_t)]


class _KillOnCloseJob:
    """Windows Job Object：父进程一死，job 里的子进程被系统杀掉。

    为什么需要它：Windows **没有**父子进程的连带死亡语义，子进程默认会活成孤儿。
    实测发生过 —— 程序被强杀（任务管理器/崩溃）时 atexit 和 finally 都不执行，
    PresentMon 就留了下来，还占着一个 ETW 会话。

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 是唯一能覆盖"父进程被强杀"的机制：
    job 句柄随进程终止被系统释放，内核随即终止 job 内的所有进程。

    失败也没关系（尽力而为）：
      · 拿不到 job 就退回原来的 atexit + finally
      · PresentMon 启动时带了 --stop_existing_session，
        下次启动会把上次残留的 ETW 会话顶掉，孤儿自己就退了
    """

    _LIMIT_KILL_ON_JOB_CLOSE = 0x2000
    _ExtendedLimitInformation = 9

    def __init__(self):
        self._k32 = None
        self.handle = None
        if sys.platform != 'win32':
            return
        try:
            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            k32.CreateJobObjectW.restype = ctypes.c_void_p
            k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
            h = k32.CreateJobObjectW(None, None)
            if not h:
                return
            info = _JOBOBJ_EXTENDED_LIMIT()
            info.BasicLimitInformation.LimitFlags = self._LIMIT_KILL_ON_JOB_CLOSE
            if not k32.SetInformationJobObject(
                    ctypes.c_void_p(h), self._ExtendedLimitInformation,
                    ctypes.byref(info), ctypes.sizeof(info)):
                k32.CloseHandle(ctypes.c_void_p(h))
                return
            self._k32 = k32
            self.handle = h
        except Exception:
            self.handle = None

    def assign(self, proc) -> bool:
        """把已启动的子进程放进 job。失败返回 False（不影响功能）。"""
        if not self.handle or proc is None:
            return False
        try:
            # Popen._handle 是 subprocess 的 Handle 包装，int() 取原始句柄值
            return bool(self._k32.AssignProcessToJobObject(
                ctypes.c_void_p(self.handle),
                ctypes.c_void_p(int(proc._handle))))
        except Exception:
            return False

    def close(self):
        if self.handle and self._k32:
            try:
                self._k32.CloseHandle(ctypes.c_void_p(self.handle))
            except Exception:
                pass
        self.handle = None


class PresentMonReader:
    """跑 PresentMon 子进程，流式读它的 CSV，缓存最近若干帧。

    接口与 RTSSReader 一致：read() -> (fps, frame_ms)。
    但与 RTSS 的"每轮同步读共享内存"不同，这里是一个**长驻子进程 + 读取线程**，
    所以自带监督/重启逻辑（PresentMon 可能因为会话冲突、权限不足、被杀而退出）。

    菜单参数说明：
        --output_stdout         CSV 走 stdout，我们流式读
        --no_console_stats      关掉控制台统计表，别污染 CSV
        --exclude_dropped       只算真正显示出来的帧（丢帧不算进帧率）
        --session_name          固定会话名，避免和用户自己的 PresentMon 抢
        --stop_existing_session 上次崩溃残留的同名会话能自动顶掉
        stderr 合并进 stdout：解析器本来就跳过噪声行，合并后还能直接读到
        "access denied" 这种关键错误（否则错误信息会丢在没人读的管道里）
    """

    RESTART_BACKOFF = [2.0, 5.0, 15.0, 30.0, 60.0]

    def __init__(self, exe_path: Optional[str]):
        self.exe_path = exe_path
        self.available = False
        self._proc = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._csv = PresentMonCsv()
        self._fails = 0
        self._denied_logged = False
        self._win_cache = None          # 可见窗口进程集合（缓存，见 _visible_ok）
        self._win_cache_t = 0.0
        # 把子进程绑进 job：父进程被强杀时它也会被系统带走（见 _KillOnCloseJob）
        self._job = _KillOnCloseJob()
        if not exe_path or sys.platform != 'win32':
            return
        self.available = True
        self._thread = threading.Thread(target=self._supervise, name='presentmon',
                                        daemon=True)
        self._thread.start()
        # 兜底：进程正常退出时也要收掉子进程。监督线程是 daemon，父进程一退
        # 它就没了，但**子进程不会被带走** —— 而 PresentMon 会占着一个 ETW 会话。
        # （调用方崩在 close() 之前也是同一种情况，所以探针那边还有一层 try/finally。）
        try:
            import atexit
            atexit.register(self.close)
        except Exception:
            pass
        log.info('PresentMon 已启动监督线程: %s', exe_path)

    def _backoff(self) -> float:
        return self.RESTART_BACKOFF[min(self._fails, len(self.RESTART_BACKOFF) - 1)]

    def _spawn(self) -> bool:
        args = [self.exe_path,
                '--output_stdout',
                '--no_console_stats',
                '--exclude_dropped',
                '--session_name', 'PCMonitor',
                '--stop_existing_session']
        try:
            self._proc = subprocess.Popen(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,      # 见类注释：合并后关键错误才读得到
                stdin=subprocess.DEVNULL,
                text=True, encoding='utf-8', errors='replace',
                bufsize=1,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            # 绑进 job（失败只是退化成"可能留孤儿"，不影响功能）
            if self._job.assign(self._proc):
                log.debug('PresentMon 已绑入 job object（父进程被强杀时会一并终止）')
            else:
                log.debug('PresentMon 未能绑入 job object，退回 atexit/finally 兜底')
            log.info('PresentMon 子进程已启动 (pid=%s)', self._proc.pid)
            return True
        except Exception as e:
            log.error('启动 PresentMon 失败: %s: %s', type(e).__name__, e)
            self._proc = None
            return False

    def _reap(self):
        p, self._proc = self._proc, None
        if p is None:
            return
        try:
            p.terminate()
        except Exception:
            pass
        try:
            p.wait(timeout=3)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass

    def _supervise(self):
        """监督循环：拉起 -> 读 stdout 直到 EOF -> 按退避重启"""
        while not self._stop.is_set():
            if not self._spawn():
                self._fails += 1
                if self._stop.wait(self._backoff()):
                    break
                continue
            try:
                for line in self._proc.stdout:
                    if self._stop.is_set():
                        break
                    low = line.lower()
                    if ('access denied' in low or 'failed to start trace session' in low) \
                            and not self._denied_logged:
                        self._denied_logged = True
                        log.error('PresentMon 起 ETW 会话被拒绝。它需要管理员权限，'
                                  '或当前用户属于 "Performance Log Users" 组'
                                  '（compmgmt.msc -> 本地用户和组 -> 组）。'
                                  '帧率将显示 --。')
                    with self._lock:
                        self._csv.feed(line)
            except Exception as e:
                log.warning('PresentMon 读取异常: %s: %s', type(e).__name__, e)
            self._reap()
            if self._stop.is_set():
                break
            if self._denied_logged:
                # 权限不足是**确定性**失败：本进程活着的期间不会自己变好
                # （用户加进 Performance Log Users 组需要重新登录，那时进程
                # 也重启了）。所以别每隔几十秒徒劳地去建一次 ETW 会话。
                log.warning('PresentMon 因权限不足停止，不再重试（帧率显示 --）')
                break
            self._fails += 1
            log.warning('PresentMon 已退出（第 %d 次），%.0fs 后重启',
                        self._fails, self._backoff())
            if self._stop.wait(self._backoff()):
                break

    def _visible_ok(self) -> set:
        """当前拥有可见窗口的进程集合（缓存 2 秒）。

        EnumWindows 一次是毫秒级，1Hz 调用完全无所谓，但没必要每帧都枚举；
        而且缓存能让"选中哪个进程"在几秒内保持稳定，屏幕上不会来回跳。
        """
        now = time.monotonic()          # 缓存有效期，用墙钟会被校时打乱
        if self._win_cache is None or (now - self._win_cache_t) > 2.0:
            self._win_cache = _visible_window_pids()
            self._win_cache_t = now
        return self._win_cache

    def read(self) -> Tuple[Optional[float], Optional[float]]:
        """返回 (fps, frametime_ms)；不可用或还没数据时 (None, None)"""
        if not self.available:
            return None, None
        allow = self._visible_ok()
        fg = _foreground_pid()
        with self._lock:
            return self._csv.read(allow, fg)

    def describe(self) -> str:
        allow = self._visible_ok() if self.available else None
        with self._lock:
            return self._csv.describe(allow, _foreground_pid())

    def low_fps(self, pct: float = 1.0) -> Optional[float]:
        """当前选中进程的 1% low 帧率（诊断用；协议里暂时没有对应字段）"""
        if not self.available:
            return None
        allow = self._visible_ok()
        fg = _foreground_pid()
        with self._lock:
            return self._csv.low_fps(pct, allow, fg)

    def close(self):
        self._stop.set()
        self._reap()
        t, self._thread = self._thread, None
        if t is not None and t.is_alive():
            t.join(timeout=3)


# ===== LibreHardwareMonitor 硬件监控 =====
class LibreHardwareMonitor:
    def __init__(self):
        self._init_state()
        self._init_fps_sources()
        self._init_libre()
        if self.initialized:
            # 预热：部分传感器需要多次 Update 才能拿到有效值
            for _ in range(3):
                self.update()
                time.sleep(0.05)

    def _init_state(self):
        """把全部缓存字段置成"还没读到"的初值（不碰硬件）。

        单独拆出来是给离线单测用的：测试可以 object.__new__ + _init_state()
        造一个没有硬件的实例，不必手工罗列三十来个属性 —— 以前漏一个就是
        AttributeError，而那种失败看着像"代码被改坏了"，其实只是测试自己过时了。
        """
        self.initialized = False
        self.computer = None

        # CPU 传感器缓存
        self.cpu_package_power = None
        self.cpu_tctl_tdie = None
        self.cpu_total_load = None
        self.cpu_clk = None
        self.cpu_volt = None
        self.cpu_core_max = None
        self.cpu_name = ""
        self._clk_hist = []  # 频率平滑历史（最近 5 次）

        # GPU 传感器缓存
        self.gpu_power = None
        self.gpu_temp = None
        self.gpu_core_load = None
        self.gpu_memory_controller_load = None
        self.gpu_memory_used = None
        self.gpu_memory_total = None
        self.gpu_hotspot = None
        self.gpu_mem_temp = None
        self.gpu_clk = None
        self.gpu_name = ""

        # 内存 / 存储 / 风扇
        self.memory_used = None
        self.hdd_temp = None
        self.disk_used = None
        self.cpu_fan = None
        self.gpu_fan = None
        self.mobo_fan = None
        self.mobo_cpu_temp = None      # 主板上的 CPU 温度（后备）

        # 系统（psutil）
        self.disk_read = RateMeter(lambda: psutil.disk_io_counters().read_bytes)
        self.disk_write = RateMeter(lambda: psutil.disk_io_counters().write_bytes)
        self.net_down = RateMeter(lambda: psutil.net_io_counters().bytes_recv)
        self.net_up = RateMeter(lambda: psutil.net_io_counters().bytes_sent)
        self.ping = PingProbe()
        # 帧率来源由 _init_fps_sources() 建立。这里保持 None 是有意的：
        # 离线单测走 _init_state() 造实例，绝不该顺手开出子进程或共享内存。
        self.rtss = None
        self.presentmon = None
        self.frame_ms: Optional[float] = None
        self._last_report_t = 0.0     # 上次"有多少字段读不到"汇总日志的时间

        # CPU 温度传感器优先级（Tctl > CCDs Max > Package > ...）
        self._cpu_temp_priority = 0

    def _init_fps_sources(self):
        """建立帧率来源（只在真正运行程序时调用，单测不调）。

        'auto' 下两个都建：谁先给出数据就用谁 —— 先 PresentMon（随包分发、
        开箱即用），再 RTSS（用户自己装了也行）。
        """
        src = Config.FPS_SOURCE
        if src not in ('auto', 'rtss', 'presentmon', 'none'):
            log.warning('FPS_SOURCE 取值不认识: %r，按 none 处理（只支持 '
                        'auto/rtss/presentmon/none）', src)
            src = 'none'

        if src in ('auto', 'rtss') and Config.RTSS_ENABLE:
            self.rtss = RTSSReader()
            log.info('帧率来源：RTSS 共享内存 %s',
                     '可用' if self.rtss.available else '不可用（RTSS 没在跑？）')

        if src in ('auto', 'presentmon'):
            path = find_presentmon_exe(Config.PRESENTMON_PATH)
            if path:
                self.presentmon = PresentMonReader(path)
            else:
                log.info('没找到 PresentMon（%s）。想要真帧率就把 %s 放到本程序'
                         '旁边，或用 ini 的 PRESENTMON_PATH 指定路径；'
                         '否则屏幕显示 --。',
                         PRESENTMON_EXE, PRESENTMON_EXE)

    def _init_libre(self) -> bool:
        """初始化 LibreHardwareMonitor"""
        try:
            dll_path = Config.LIBRE_DLL_PATH
            if not os.path.exists(dll_path):
                log.error('LibreHardwareMonitor dll 不存在: %s', dll_path)
                return False

            import clr
            clr.AddReference(dll_path)
            from LibreHardwareMonitor import Hardware

            self.computer = Hardware.Computer()
            self.computer.IsCpuEnabled = True
            self.computer.IsGpuEnabled = True
            self.computer.IsMemoryEnabled = True
            self.computer.IsFanControllerEnabled = True
            self.computer.IsStorageEnabled = True
            self.computer.IsMotherboardEnabled = True

            self.computer.Open()
            self.initialized = True
            log.info('LibreHardwareMonitor 初始化成功: %s', dll_path)
            return True
        except Exception as e:
            log.exception('LibreHardwareMonitor 初始化失败: %s: %s', type(e).__name__, e)
            return False

    def _update_hardware(self, hardware):
        """更新单个硬件及其子硬件"""
        try:
            hardware.Update()
        except:
            pass
        try:
            for sub in hardware.SubHardware:
                try:
                    sub.Update()
                except:
                    pass
        except:
            pass

    def update(self):
        """更新所有传感器数据（一次遍历里把需要的都取出来，避免重复 Update）"""
        if not self.initialized or not self.computer:
            return

        # 清空缓存（型号名不清，名称不常变）
        self.cpu_package_power = None
        self.cpu_tctl_tdie = None
        self.cpu_total_load = None
        self.cpu_clk = None
        self.cpu_volt = None
        self.cpu_core_max = None
        self.gpu_power = None
        self.gpu_temp = None
        self.gpu_core_load = None
        self.gpu_memory_controller_load = None
        self.gpu_memory_used = None
        self.gpu_memory_total = None
        self.gpu_hotspot = None
        self.gpu_mem_temp = None
        self.gpu_clk = None
        self.memory_used = None
        self.hdd_temp = None
        self.cpu_fan = None
        self.gpu_fan = None
        self.mobo_fan = None
        self.mobo_cpu_temp = None
        self._cpu_temp_priority = 0

        gpu_candidates = []
        try:
            for hardware in self.computer.Hardware:
                self._update_hardware(hardware)
                hardware_type = str(hardware.HardwareType)

                if hardware_type == 'Cpu':
                    try:
                        self.cpu_name = str(hardware.Name)
                    except Exception:
                        pass
                    self._parse_cpu(hardware)
                elif hardware_type in ('GpuNvidia', 'GpuAmd', 'GpuIntel'):
                    gpu_candidates.append((hardware_type, hardware))
                elif hardware_type == 'Memory':
                    self._parse_memory(hardware)
                elif hardware_type == 'Storage':
                    self._parse_storage(hardware)
                elif hardware_type in ('Motherboard', 'SuperIO'):
                    self._parse_board(hardware)

            # GPU 优先级：NVIDIA > AMD > Intel，取第一个有有效数据的 GPU
            for prio in ('GpuNvidia', 'GpuAmd', 'GpuIntel'):
                for hw_type, hw in gpu_candidates:
                    if hw_type == prio:
                        try:
                            self.gpu_name = str(hw.Name)
                        except Exception:
                            pass
                        self._parse_gpu(hw)
                if self.gpu_temp is not None or self.gpu_core_load is not None:
                    break

            # 系统盘占用率（psutil，零成本）
            try:
                self.disk_used = psutil.disk_usage(os.environ.get('SystemDrive', 'C:') + '\\').percent
            except Exception:
                self.disk_used = None
        except Exception as e:
            # 注意：update() 开头已经把缓存清空了，这里异常意味着被清掉却没回填的
            # 字段这一轮全是 None（屏幕显示 --）。以前静默吞掉，现在至少留痕。
            log.warning('传感器刷新异常（本轮部分字段会显示 --）: %s: %s',
                        type(e).__name__, e)

    @staticmethod
    def _valid_temp(v) -> bool:
        """温度有效性：必须大于 0 且不超过 200°C"""
        try:
            return v is not None and 0 < float(v) < 200
        except:
            return False

    @staticmethod
    def _valid_power(v) -> bool:
        """功耗有效性：必须大于 0 且不超过 2000W"""
        try:
            return v is not None and 0 < float(v) < 2000
        except:
            return False

    @staticmethod
    def _valid_load(v) -> bool:
        """负载有效性：0~100%"""
        try:
            return v is not None and 0 <= float(v) <= 100
        except:
            return False

    @staticmethod
    def _valid_volt(v) -> bool:
        """电压有效性：0.3~3.0V（CPU 核心电压的合理区间）"""
        try:
            return v is not None and 0.3 < float(v) < 3.0
        except:
            return False

    @staticmethod
    def _cpu_temp_priority_of(name: str) -> int:
        """CPU 温度传感器优先级：
        Tctl/Tdie > CCDs Max > Package > 其他 CPU/CCD"""
        if 'Tctl' in name:
            return 100
        if 'CCDs Max' in name:
            return 90
        if 'Package' in name:
            return 80
        if 'Core Max' in name or 'Max Core' in name:
            return 75
        if 'CPU' in name:
            return 70
        if 'Tdie' in name:
            return 60
        return 0

    def _parse_cpu(self, hardware):
        """解析CPU传感器（含子硬件）"""
        def parse(hw):
            for sensor in hw.Sensors:
                try:
                    name = sensor.Name
                    value = sensor.Value
                    sensor_type = str(sensor.SensorType)

                    if value is None:
                        continue
                    value = float(value)

                    # ---- 温度：按优先级取最高（避免 CCD 温度覆盖 Tctl） ----
                    if sensor_type == 'Temperature' and self._valid_temp(value):
                        p = self._cpu_temp_priority_of(name)
                        if p > self._cpu_temp_priority:
                            self.cpu_tctl_tdie = value
                            self._cpu_temp_priority = p

                    # ---- 功耗：优先 Package，其次 SMU/Total ----
                    if sensor_type == 'Power' and self._valid_power(value):
                        if 'Package' in name:
                            self.cpu_package_power = value
                        elif self.cpu_package_power is None and \
                             ('SMU' in name or 'Total' in name):
                            self.cpu_package_power = value

                    # ---- 负载：总计 + 单核最大 ----
                    if sensor_type == 'Load' and self._valid_load(value):
                        if 'Total' in name or 'Package' in name:
                            self.cpu_total_load = value
                        elif 'Core' in name and 'Max' not in name:
                            # "CPU Core #1" 这类单核负载：取最忙的那个
                            if self.cpu_core_max is None or value > self.cpu_core_max:
                                self.cpu_core_max = value

                    # ---- 频率：Cores (Average Effective)（MHz，实际运行频率） ----
                    if sensor_type == 'Clock' and 100 < value < 20000:
                        if 'Effective' in name and 'Average' in name and 'Cores' in name:
                            self.cpu_clk = value

                    # ---- 电压 ----
                    # 台式机常见 "CPU Core" / "Vcore" / "Core Voltage"；
                    # 但 AMD 笔记本/锐龙报的是逐核的 "Core #1 VID"（实测
                    # ASUS G614FP + Ryzen 9 9955HX 只有这一种）。
                    # 原来的匹配没覆盖 VID，导致 cpu_volt 永远读不到 ——
                    # 这类"按名字匹配传感器"的代码天生脆，加传感器时容易漏。
                    # 优先级：实测值（CPU Core/Vcore）> 逐核 VID（请求值）。
                    if sensor_type == 'Voltage' and self._valid_volt(value):
                        if ('CPU Core' in name or 'Vcore' in name
                                or 'Core Voltage' in name):
                            self.cpu_volt = value          # 实测值，总是覆盖
                        elif 'VID' in name and self.cpu_volt is None:
                            self.cpu_volt = value          # 退而求其次
                except:
                    pass
        parse(hardware)
        try:
            for sub in hardware.SubHardware:
                parse(sub)
        except:
            pass

    def _parse_gpu(self, hardware):
        """解析GPU传感器（含子硬件）"""
        def parse(hw):
            for sensor in hw.Sensors:
                try:
                    name = sensor.Name
                    value = sensor.Value
                    sensor_type = str(sensor.SensorType)

                    if value is None:
                        continue
                    value = float(value)

                    if sensor_type == 'Power' and self._valid_power(value):
                        if 'Package' in name:
                            self.gpu_power = value

                    if sensor_type == 'Temperature' and self._valid_temp(value):
                        if name == 'GPU Core':
                            self.gpu_temp = value
                        elif 'Hot Spot' in name:
                            self.gpu_hotspot = value
                        elif 'Memory Junction' in name or 'GPU Memory' in name:
                            self.gpu_mem_temp = value

                    if sensor_type == 'Load' and self._valid_load(value):
                        if name == 'GPU Core':
                            self.gpu_core_load = value
                        if 'Memory Controller' in name:
                            self.gpu_memory_controller_load = value

                    if sensor_type == 'Clock' and 10 < value < 6000:
                        if name == 'GPU Core':
                            self.gpu_clk = value

                    if sensor_type == 'SmallData':
                        if 'GPU Memory Used' in name:
                            self.gpu_memory_used = value / 1024
                        if 'GPU Memory Total' in name:
                            self.gpu_memory_total = value / 1024

                    if sensor_type == 'Fan' and value > 0:
                        if self.gpu_fan is None:
                            self.gpu_fan = int(value)
                except:
                    pass
        parse(hardware)
        try:
            for sub in hardware.SubHardware:
                parse(sub)
        except:
            pass

    def _parse_memory(self, hardware):
        """解析内存传感器"""
        for sensor in hardware.Sensors:
            try:
                name = sensor.Name
                value = sensor.Value
                sensor_type = str(sensor.SensorType)

                if value is None:
                    continue

                if name == 'Memory' and sensor_type == 'Load':
                    self.memory_used = float(value)
            except:
                pass

    def _parse_storage(self, hardware):
        """解析存储设备传感器：温度取所有盘里最高的，避免被某块不报温度的盘挡住"""
        for sensor in hardware.Sensors:
            try:
                name = sensor.Name
                value = sensor.Value
                sensor_type = str(sensor.SensorType)

                if value is None:
                    continue

                if sensor_type == 'Temperature' and self._valid_temp(value):
                    v = float(value)
                    if self.hdd_temp is None or v > self.hdd_temp:
                        self.hdd_temp = v
            except:
                pass

    def _parse_board(self, hardware):
        """解析主板/SuperIO：CPU 风扇、机箱风扇、主板上的 CPU 温度（后备）

        风扇命名各家不同（"CPU Fan" / "Fan #1" / "Chassis Fan"），所以先扫出所有风扇，
        再按名字认 CPU 风扇；没有命名的就把第一个当 CPU、第二个当机箱，
        避免"主板只报扇子不报名字"时两个字段都空着。
        """
        fans = []

        def collect(hw):
            for sensor in hw.Sensors:
                try:
                    value = sensor.Value
                    if value is None:
                        continue
                    value = float(value)
                    if str(sensor.SensorType) == 'Fan' and value > 0:
                        fans.append((sensor.Name, int(value)))
                    if str(sensor.SensorType) == 'Temperature' and self._valid_temp(value):
                        n = sensor.Name
                        if self.mobo_cpu_temp is None and \
                           ('CPU' in n or 'Tctl' in n or 'Tdie' in n or 'Package' in n):
                            self.mobo_cpu_temp = value
                except:
                    pass

        collect(hardware)
        try:
            for sub in hardware.SubHardware:
                collect(sub)
        except:
            pass

        for name, rpm in fans:
            if self.cpu_fan is None and ('CPU' in name or 'Cpu' in name):
                self.cpu_fan = rpm
        for name, rpm in fans:
            if self.cpu_fan is None:
                self.cpu_fan = rpm
                continue
            if self.mobo_fan is None and rpm != self.cpu_fan:
                self.mobo_fan = rpm

    def get_cpu_info(self):
        """获取CPU信息: 负载, 温度, 风扇, 功耗（读不到返回 None，不造假数据）

        读的是 update() 填好的传感器缓存，调用前必须已经 self.update()
        （由 collect_all_data 统一负责，不要在这里再刷新一次）。
        """
        # ---- CPU 负载：真实值优先，回退到系统级负载 ----
        cpu_load = self.cpu_total_load
        if cpu_load is None:
            try:
                cpu_load = psutil.cpu_percent(interval=0.2)
            except:
                cpu_load = None

        # ---- CPU 温度：过滤 0/无效值，必要时用主板上的 CPU 温度兜底 ----
        cpu_temp = self.cpu_tctl_tdie
        if cpu_temp is None or not self._valid_temp(cpu_temp):
            cpu_temp = self.mobo_cpu_temp

        # ---- CPU 功耗：过滤 0/无效值 ----
        cpu_power = self.cpu_package_power
        if cpu_power is not None and not self._valid_power(cpu_power):
            cpu_power = None

        # ---- CPU 风扇（已在 _parse_board 里取到）----
        cpu_fan = self.cpu_fan

        return cpu_load, cpu_temp, cpu_fan, cpu_power

    def get_gpu_info(self):
        """获取GPU信息: 负载, 温度, 显存, 风扇, 功耗（读不到返回 None）

        读的是 update() 填好的传感器缓存，调用前必须已经 self.update()。
        """
        gpu_load = self.gpu_core_load
        if gpu_load is not None and not self._valid_load(gpu_load):
            gpu_load = None

        gpu_temp = self.gpu_temp
        if gpu_temp is not None and not self._valid_temp(gpu_temp):
            gpu_temp = None

        gpu_mem_used = self.gpu_memory_used

        gpu_fan = self.gpu_fan

        gpu_power = self.gpu_power
        if gpu_power is not None and not self._valid_power(gpu_power):
            gpu_power = None

        return gpu_load, gpu_temp, gpu_mem_used, gpu_fan, gpu_power

    def get_system_info(self):
        """获取系统信息: 内存使用率, 硬盘温度（读不到返回 None）

        读的是 update() 填好的传感器缓存，调用前必须已经 self.update()。
        """
        mem_used = self.memory_used
        if mem_used is None:
            try:
                mem_used = psutil.virtual_memory().percent
            except:
                mem_used = None

        hdd_temp = self.hdd_temp
        if hdd_temp is not None and not self._valid_temp(hdd_temp):
            hdd_temp = None

        return mem_used, hdd_temp

    def get_fps(self) -> Tuple[Optional[float], Optional[float]]:
        """获取帧率与帧时间（都是**实测**值）；两个来源都拿不到就 (None, None)。

        来源由 Config.FPS_SOURCE 决定；拿不到就上报"无效"，协议发 0xFFFF，
        屏幕显示 "--"。

        **这里绝不估算。** 以前有一条兜底：fps = 60 * (1 - gpu_load/100 * 0.4)，
        已删除，两个理由：
          1) 它是编的 —— 跟真实帧率没有任何关系，算出来恒定落在 36~60，还随负载
             平滑变化，屏幕上看就是个非常可信的假读数。一块监控屏只要有一个
             数字是假的，其它数字也就没人信了。
          2) 它连"参考价值"都没有 —— 该式是 gpu_load 的仿射函数（fps = 60 - 0.24L），
             而负载本来就显示在同一块屏上；给定负载这个"帧率"完全确定，信息量为零。
        宁可显示 "--"。
        """
        if self.presentmon is not None:
            fps, frame_ms = self.presentmon.read()
            if fps is not None:
                return fps, frame_ms
        if self.rtss is not None:
            fps, frame_ms = self.rtss.read()
            if fps is not None:
                return fps, frame_ms
        return None, None

    def get_fps_low(self) -> Optional[float]:
        """1% low 帧率（最慢的 1% 帧的平均帧率）；拿不到返回 None。

        为什么单独一个字段、而且由 PC 端算：
          · "平均帧率"和"1% low"回答的是两个不同问题。实测出现过平均 57fps、
            1% low 只有 12fps —— 平均看着还行，实际一顿一顿的。
          · 必须在**逐帧**数据上算，所以只有 PresentMon 这条路给得出。
            RTSS 那条路只有帧计数增量，没有逐帧数据，只能返回 None。
          · 固件那边原来显示的是"140 秒环形缓冲里最差的那一个采样"，
            那是**单帧极值**（min），一颗老鼠屎在屏幕上挂两分多钟；
            1% low 抗离群点，才是业界通用的卡顿指标。
        """
        if self.presentmon is not None:
            v = self.presentmon.low_fps(1.0)
            if v is not None:
                return v
        return None

    def collect_all_data(self) -> Dict[str, Any]:
        """采集所有数据（读不到的值保持 None，打包时变成无效哨兵）

        整机传感器只读一遍：下面三个 get_* 和末尾直接读缓存的字段
        （cpu_clk / gpu_hotspot / gpu_clk / mobo_fan …）共用这一份快照。
        原先三个 get_* 各自调一次 self.update()，同一份数据白读三遍 ——
        除了白烧 CPU，还会把采集周期拖长，逼近固件 3 秒的 DATA_TIMEOUT_MS
        （屏幕会判定"链路无数据"整片置灰，看着像坏了）。
        """
        self.update()
        cpu_load, cpu_temp, cpu_fan, cpu_power = self.get_cpu_info()
        gpu_load, gpu_temp, gpu_mem, gpu_fan, gpu_power = self.get_gpu_info()
        mem_used, hdd_temp = self.get_system_info()
        fps, frame_ms = self.get_fps()
        self.frame_ms = frame_ms
        fps_low = self.get_fps_low()

        # CPU 频率（MHz）：取最近 5 次采样平均，避免瞬时抖动
        raw_clk = self.cpu_clk
        if raw_clk is not None:
            self._clk_hist.append(raw_clk)
            if len(self._clk_hist) > 5:
                self._clk_hist.pop(0)
            cpu_clk = sum(self._clk_hist) / len(self._clk_hist)
        else:
            self._clk_hist.clear()
            cpu_clk = None

        # 派生速率
        disk_read = self.disk_read.sample()
        disk_write = self.disk_write.sample()
        net_down = self.net_down.sample()
        net_up = self.net_up.sample()

        # 进程数 / 运行时长
        try:
            proc_count = len(psutil.pids())
        except Exception:
            proc_count = None
        try:
            # 注意这里**必须**用墙钟：psutil.boot_time() 返回的是 epoch 时刻，
            # 两个 epoch 相减才是开机时长；换成 monotonic 会得到一个没有意义的数。
            uptime_min = (time.time() - psutil.boot_time()) / 60.0
        except Exception:
            uptime_min = None

        out = {
            # 字段顺序必须与 PROTOCOL.md 一致
            'cpu_load': cpu_load,
            'cpu_temp': cpu_temp,
            'cpu_fan': cpu_fan,
            'cpu_power': cpu_power,
            'gpu_load': gpu_load,
            'gpu_temp': gpu_temp,
            'gpu_mem': gpu_mem,
            'gpu_fan': gpu_fan,
            'gpu_power': gpu_power,
            'fps': fps,
            'mem_used': mem_used,
            'hdd_temp': hdd_temp,
            'gpu_mem_total': self.gpu_memory_total,
            'cpu_clk': cpu_clk,
            'cpu_volt': self.cpu_volt,
            'cpu_core_max': self.cpu_core_max,
            'gpu_hotspot': self.gpu_hotspot,
            'gpu_mem_temp': self.gpu_mem_temp,
            'gpu_clk': self.gpu_clk,
            'gpu_mem_ctrl': self.gpu_memory_controller_load,
            'mobo_fan': self.mobo_fan,
            'disk_read': _fit_field('disk_read', disk_read),
            'disk_write': _fit_field('disk_write', disk_write),
            'disk_used': self.disk_used,
            'net_down': _fit_field('net_down', net_down),
            'net_up': _fit_field('net_up', net_up),
            'ping_ms': self.ping.ms,
            'ping_loss': self.ping.loss,
            'proc_count': proc_count,
            'uptime_min': _fit_field('uptime_min', uptime_min),
            'frame_ms': frame_ms,
            'fps_low': fps_low,
        }

        # 周期性自检：报告有几个字段读不到。
        # 硬件传感器改名（驱动/固件更新后很常见）会让某些字段永久变成 --，
        # 而程序照常运行、照常发送 —— 这种"静默降级"以前完全看不出来。
        now = time.monotonic()          # 只用来算间隔，墙钟跳变会让自检忽快忽慢
        if now - self._last_report_t >= 60.0:
            self._last_report_t = now
            missing = [k for k, v in out.items() if v is None]
            if missing:
                log.info('传感器自检：%d/%d 个字段读不到 -> %s',
                         len(missing), len(out), ', '.join(missing))
            else:
                log.info('传感器自检：%d 个字段全部读到', len(out))
        return out

    # ---- 打包 ----

    def pack_binary(self, data: Dict[str, Any], seq: int) -> bytes:
        """二进制定标编码：包头 + 序号 + 长度 + 31×uint16 + CRC8"""
        return pack_binary(data, seq)

    def pack_names(self) -> bytes:
        """名称包：'CPU型号|GPU型号'（型号名在这里缩短，屏幕副标题位置有限）"""
        return _pack_names(short_name(self.cpu_name), short_name(self.gpu_name))

    def pack_test(self) -> bytes:
        """连接后的握手包：**全部字段填无效哨兵（0xFFFF）**，用来确认协议通。

        以前这里发的是一组手写的"演示值"（cpu_load 50%、cpu_temp 45°C、fps 60…），
        理由是"给一组可信的测试值"。但那意味着每次连接/重连，屏幕上都会闪一帧
        编造出来的读数 —— 和本项目"只显示实测值，拿不到就显示 --"的原则直接冲突
        （见 PROTOCOL.md §4 与 pc_side/README.md 的"真帧率"一节）。
        发空包同样能确认通路（固件收到后会正常解析、推进序号），但一个假数字都不显示。
        """
        return self.pack_binary({}, 0)

    def pack_payload(self, data: Dict[str, Any], seq: int):
        """按 Config.PROTOCOL 选择编码方式"""
        if Config.PROTOCOL == 'csv':
            return self.format_for_esp32(data)
        return self.pack_binary(data, seq)

    @staticmethod
    def _fmt(v, spec: str) -> str:
        """旧 CSV 协议用：None 返回 0.0（固件把 0 当 --）"""
        if v is None:
            return spec.format(0.0)
        try:
            return spec.format(v)
        except:
            return spec.format(0.0)

    def format_for_esp32(self, data: Dict[str, Any]) -> str:
        """旧协议：14 个字段的 CSV 文本（仅用于 Config.PROTOCOL == 'csv' 回退）"""
        return (f"{self._fmt(data['cpu_load'], '{:.1f}')},{self._fmt(data['cpu_temp'], '{:.1f}')},"
                f"{self._fmt(data['cpu_fan'], '{:.0f}')},{self._fmt(data['cpu_power'], '{:.1f}')},"
                f"{self._fmt(data['gpu_load'], '{:.1f}')},{self._fmt(data['gpu_temp'], '{:.1f}')},"
                f"{self._fmt(data['gpu_mem'], '{:.2f}')},{self._fmt(data['gpu_fan'], '{:.0f}')},"
                f"{self._fmt(data['gpu_power'], '{:.1f}')},{self._fmt(data['fps'], '{:.0f}')},"
                f"{self._fmt(data['mem_used'], '{:.1f}')},{self._fmt(data['hdd_temp'], '{:.1f}')},"
                f"{self._fmt(data['gpu_mem_total'], '{:.1f}')},"
                f"{self._fmt(data['cpu_clk'], '{:.0f}')}")


# ===== BLE 发送器 =====

class BLESender:
    def __init__(self):
        self.device_name = Config.DEVICE_NAME
        self.char_uuid = Config.CHAR_WRITE_UUID
        self.client = None
        self.device = None
        self.running = False
        self.reconnect_attempts = 0
        self.stats = {'sent': 0, 'failed': 0}
        # 下面所有"过了多久"的判断都拿这两个做差，所以必须用单调钟：
        # 墙钟被 NTP 校时/跨夏令时一跳，差值会变成负数（超时永不触发，程序再也
        # 不会自己退出）或者突然很大（立刻触发自动退出）。
        self.last_success_time = time.monotonic()

    async def find_device(self) -> bool:
        try:
            log.info('扫描 BLE 设备 "%s"（超时 %ss）…', self.device_name, Config.SCAN_TIMEOUT)
            self.device = await BleakScanner.find_device_by_name(self.device_name, timeout=Config.SCAN_TIMEOUT)
            if self.device:
                log.info('已找到设备: %s', self.device)
                return True
            log.warning('扫描不到设备 "%s"：ESP32 是否已上电并在广播？', self.device_name)
            return False
        except Exception as e:
            log.exception('扫描 BLE 设备异常: %s: %s', type(e).__name__, e)
            return False

    async def connect(self) -> bool:
        if not self.device:
            # 没有（或已失效的）device 就先扫一次。重连路径尤其需要这一步：
            # ESP32 每次重启会换成随机 BLE 地址（没配固定地址的话），抱着上一次的
            # device 对象重试**永远不会成功** —— 表现是"重连 3 次后程序退出"，
            # 而设备其实就在旁边好好广播着。
            if not await self.find_device():
                return False
        try:
            self.client = BleakClient(self.device, timeout=Config.CONNECT_TIMEOUT)
            await self.client.connect()
            if self.client.is_connected:
                self.reconnect_attempts = 0
                self.last_success_time = time.monotonic()  # 连接成功更新最后成功时间
                log.info('BLE 已连接')
                return True
            log.warning('连接返回但 is_connected 为 False')
            return False
        except Exception as e:
            log.warning('BLE 连接失败: %s: %s', type(e).__name__, e)
            return False

    async def disconnect(self):
        if self.client and self.client.is_connected:
            try:
                await self.client.disconnect()
                log.info('BLE 已断开')
            except:
                pass

    async def send_data(self, payload) -> bool:
        """payload 可以是 bytes（二进制协议）或 str（旧 CSV 协议）"""
        if not self.client or not self.client.is_connected:
            return False
        try:
            if isinstance(payload, str):
                payload = payload.encode()
            await self.client.write_gatt_char(self.char_uuid, payload)
            self.stats['sent'] += 1
            self.last_success_time = time.monotonic()  # 发送成功更新最后成功时间
            return True
        except Exception as e:
            self.stats['failed'] += 1
            n = self.stats['failed']
            # 头几次逐条记，之后每 50 次记一次，免得日志被刷爆
            if n <= 3 or n % 50 == 0:
                log.warning('发送失败（累计 %d 次）: %s: %s', n, type(e).__name__, e)
            return False

    async def run(self, monitor: LibreHardwareMonitor):
        # 首次连接：扫描和连接都**重试**，总预算仍是 AUTO_EXIT_MINUTES。
        # 旧行为是"扫一次，扫不到就干等 10 分钟然后退出"：ESP32 只是晚开机几秒、
        # 或者在重启/刚刷完固件，程序就白白退出，用户看到的是"程序自己关了"。
        deadline = time.monotonic() + Config.AUTO_EXIT_MINUTES * 60
        while not await self.find_device():
            if time.monotonic() >= deadline:
                log.error('连续 %d 分钟扫描不到设备 "%s"，退出（ESP32 是否已上电并在广播？）',
                          Config.AUTO_EXIT_MINUTES, self.device_name)
                return
            log.info('10 秒后重新扫描…')
            await asyncio.sleep(10)

        while not await self.connect():
            if time.monotonic() >= deadline:
                log.error('连续 %d 分钟连不上设备 "%s"，退出',
                          Config.AUTO_EXIT_MINUTES, self.device_name)
                return
            log.info('10 秒后重试连接…')
            await asyncio.sleep(10)

        # 网络延迟探测放后台跑（不阻塞采集）
        ping_task = asyncio.create_task(monitor.ping.run())

        # 连接后先发型号名，再发一个"全无效"的握手包确认协议通（不显示任何编造读数）
        await self.send_data(monitor.pack_names())
        names_sent_at = time.monotonic()
        await self.send_data(monitor.pack_test())
        await asyncio.sleep(1)

        self.running = True
        seq = 0

        while self.running:
            try:
                # 检查是否超时（AUTO_EXIT_MINUTES 分钟没有任何成功通信）
                elapsed = time.monotonic() - self.last_success_time
                if elapsed > Config.AUTO_EXIT_MINUTES * 60:
                    # 超时，自动退出
                    log.error('连续 %d 分钟没有任何成功通信，自动退出'
                              '（已发 %d 包，失败 %d 包）',
                              Config.AUTO_EXIT_MINUTES,
                              self.stats['sent'], self.stats['failed'])
                    break

                # 检查连接状态，断线重连
                if not self.client or not self.client.is_connected:
                    if self.reconnect_attempts >= Config.MAX_RECONNECT_ATTEMPTS:
                        log.error('重连 %d 次仍失败，放弃并退出',
                                  Config.MAX_RECONNECT_ATTEMPTS)
                        break
                    self.reconnect_attempts += 1
                    log.warning('连接已断开，第 %d/%d 次重连…',
                                self.reconnect_attempts, Config.MAX_RECONNECT_ATTEMPTS)
                    if await self.connect():
                        # 重连后补发一次型号名
                        await self.send_data(monitor.pack_names())
                        names_sent_at = time.monotonic()
                        continue
                    # 重连失败：丢掉旧的 device，下一轮 connect() 会重新扫描。
                    # ESP32 每次重启换随机 BLE 地址，抱着旧对象重试永远不会成功。
                    self.device = None
                    await asyncio.sleep(2)
                    continue

                # 型号名低频重发（屏幕重连/重启后会用到）
                if time.monotonic() - names_sent_at > Config.NAMES_RESEND_SEC:
                    await self.send_data(monitor.pack_names())
                    names_sent_at = time.monotonic()

                # 采集并发送数据
                data = monitor.collect_all_data()
                await self.send_data(monitor.pack_payload(data, seq))
                seq = (seq + 1) & 0xFF

                await asyncio.sleep(Config.COLLECT_INTERVAL)

            except KeyboardInterrupt:
                log.info('收到 Ctrl-C，退出')
                break
            except Exception as e:
                log.exception('发送循环异常（1 秒后继续）')
                await asyncio.sleep(1)

        ping_task.cancel()
        await self.disconnect()


# 注：原来这里还有一个 _wait_for_timeout_or_exit()：扫不到设备/连不上之后
# "干等到 AUTO_EXIT_MINUTES 才退出，期间不做任何重试"。改成在上面 run() 里
# 按 10 秒间隔重试扫描与连接（总预算仍是 AUTO_EXIT_MINUTES）之后，它没有调用者了。


# ===== 主函数 =====

async def main():
    monitor = None
    try:
        # 检查DLL是否存在
        dll_path = Config.LIBRE_DLL_PATH
        if not os.path.exists(dll_path):
            _fatal('找不到硬件监控库，程序无法工作：\n{}\n\n'
                   '请确认程序目录下存在 dlls\\LibreHardwareMonitorLib.dll。'.format(dll_path))
            return

        # 初始化硬件监控
        monitor = LibreHardwareMonitor()
        if not monitor.initialized:
            _fatal('硬件监控库初始化失败，所有读数都会是 --。\n\n'
                   '常见原因：缺少 .NET Framework 运行库、dlls 目录不完整、\n'
                   '或 clr(pythonnet) 没能加载。\n\n'
                   '详细日志：{}'.format(LOG_PATH))
            return

        # 智能提权。两种情形需要管理员：
        #   1) LHM 读 AMD 的 Tctl/Package（Intel 普通权限就能读，不该白弹 UAC）
        #   2) PresentMon 起 ETW 会话 —— 实测非管理员直接失败：
        #        error: failed to start trace session: access denied.   (exit 6)
        #      除非当前用户属于 "Performance Log Users" 组。
        # 所以只要真的启用了 PresentMon 就值得提权，不能只在"读不到 CPU 温度"时才提。
        need_admin = (monitor.cpu_tctl_tdie is None or monitor.cpu_package_power is None)
        why = 'CPU 温度/功耗需要管理员权限才能读取'
        if (not need_admin and monitor.presentmon is not None
                and monitor.presentmon.available):
            need_admin = True
            why = 'PresentMon 起 ETW 会话需要管理员权限'
        if need_admin:
            log.info('需要提权：%s', why)
            if ensure_admin():
                log.info('已启动提权实例，本进程退出')
                return
            log.warning('提权被取消或失败，继续以普通权限运行（%s）', why)

        log.info('采集器就绪（协议=%s，帧率来源=%s，采集间隔=%.2fs，目标设备=%s）',
                 Config.PROTOCOL, Config.FPS_SOURCE, Config.COLLECT_INTERVAL,
                 Config.DEVICE_NAME)

        # 启动BLE发送器（静默运行）
        sender = BLESender()
        await sender.run(monitor)
        log.info('发送器结束：发出 %d 包，失败 %d 包', sender.stats['sent'], sender.stats['failed'])

    except Exception as e:
        log.exception('主流程异常退出')
        _fatal('PC Monitor 异常退出：\n{}: {}\n\n详细日志：{}'.format(
            type(e).__name__, e, LOG_PATH))
    finally:
        # 别把 PresentMon 子进程留成孤儿（它也占着一个 ETW 会话）
        if monitor is not None and monitor.presentmon is not None:
            try:
                monitor.presentmon.close()
            except Exception:
                pass


if __name__ == "__main__":
    asyncio.run(main())
