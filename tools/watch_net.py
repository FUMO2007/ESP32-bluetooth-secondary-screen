# -*- coding: utf-8 -*-
r"""实时对照工具：把 PC 端每秒算出的网络速率，连同"固件那侧应该显示成什么"
   一起打出来，方便和屏幕上的 NET 卡片逐行对照。

   每行三个信息：
     ① 真实速率（PC 端 RateMeter 算出来的 MB/s，换算成 KB/s 或 MB/s 显示）
     ② raw = 打包进 BLE 包的整数（新协议 x100）
     ③ "旧固件会显示" = 如果对面固件还是 ÷10 的老版本，屏幕上会是这个数
        —— 屏幕上显示的数字对上哪个，就知道是不是两端刻度不一致。

   用法一：双击 tools\watch_net.bat（最省事，不需要管理员权限）
   用法二：在项目根目录 PC_Monitor_1 下执行
     D:\\PC_Monitor\\PC端\\venv\\Scripts\\python.exe -B tools\\watch_net.py
   按 Ctrl+C 停止。
"""
import os
import sys
import time

os.environ['PCMON_NO_HIDE'] = '1'
sys.path.insert(0, r'D:\PC_Monitor\PC端')

# 控制台编码千奇百怪（GBK / UTF-8 / 437），中文表头编不出来时降级，别让脚本直接崩
try:
    sys.stdout.reconfigure(errors='replace')
except Exception:
    pass

import psutil                                        # noqa: E402
import pc_monitor_libre as m                          # noqa: E402
import proto_v2 as pv                                 # noqa: E402

IDX = {'DOWN': 24, 'UP': 25}


def show(v, idx):
    """返回（固件应显示的文字, raw 整数）"""
    raw = pv.pack_field(v, idx)
    if raw == pv.PROTO_INVALID:
        return '--', raw
    mbs = raw / float(pv.FIELD_SCALE[idx])
    if mbs < 0.9995:
        return '%d KB/s' % round(mbs * 1024.0), raw
    return '%.1f MB/s' % mbs, raw


def main():
    meters = {
        'DOWN': m.RateMeter(lambda: psutil.net_io_counters().bytes_recv),
        'UP':   m.RateMeter(lambda: psutil.net_io_counters().bytes_sent),
    }
    print('对照工具已启动（Ctrl+C 停止）')
    print('  %-6s %-11s %-6s %-14s %s' % ('方向', '真实速率', 'raw', '新固件显示', '旧固件(÷10)会显示'))
    while True:
        for k, mt in meters.items():
            v = mt.sample()
            txt, raw = show(v, IDX[k])
            old = ('--' if raw == pv.PROTO_INVALID else '%.1f MB/s' % (raw / 10.0))
            print('  %-6s %-11s %-6s %-14s %s' % (k, '%.3f MB/s' % v, raw, txt, old), flush=True)
        time.sleep(1.0)


try:
    main()
except KeyboardInterrupt:
    print('\n已停止')
