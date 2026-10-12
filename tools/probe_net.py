# -*- coding: utf-8 -*-
"""网络/磁盘速率探针：用 PC 端真实的 RateMeter + proto_v2 打包逻辑，
   看累计字节数 -> MB/s -> uint16 定标值 这条链路到底给出什么。

   看点：
     · net_down/net_up 的缩放是 x10（0.1 MB/s 一格），
       所以 < 0.05 MB/s（约 51 KB/s）会被 round 成 raw=0，屏幕就显示 0.0。
     · 第一次 sample() 必然返回 0（还没有上一个计数点）。
"""
import os
import sys
import time

os.environ['PCMON_NO_HIDE'] = '1'
sys.path.insert(0, r'D:\PC_Monitor\PC端')

import psutil                                        # noqa: E402
import pc_monitor_libre as m                          # noqa: E402
import proto_v2 as pv                                 # noqa: E402

MB = 1024.0 * 1024.0
IDX = {'net_down': 24, 'net_up': 25, 'disk_read': 21, 'disk_write': 22}

meters = {
    'net_down':   m.RateMeter(lambda: psutil.net_io_counters().bytes_recv),
    'net_up':     m.RateMeter(lambda: psutil.net_io_counters().bytes_sent),
    'disk_read':  m.RateMeter(lambda: psutil.disk_io_counters().read_bytes),
    'disk_write': m.RateMeter(lambda: psutil.disk_io_counters().write_bytes),
}
raw_get = {
    'net_down':   lambda: psutil.net_io_counters().bytes_recv,
    'net_up':     lambda: psutil.net_io_counters().bytes_sent,
    'disk_read':  lambda: psutil.disk_io_counters().read_bytes,
    'disk_write': lambda: psutil.disk_io_counters().write_bytes,
}

print('t  %-11s %-11s %-11s %-11s' % ('net_down', 'net_up', 'disk_read', 'disk_write'))
print('   (MB/s / raw / 屏幕)')
for i in range(15):
    row = []
    for k in ('net_down', 'net_up', 'disk_read', 'disk_write'):
        v = meters[k].sample()
        raw = pv.pack_field(v, IDX[k])
        shown = '--' if raw == pv.PROTO_INVALID else ('%.1f' % (raw / pv.FIELD_SCALE[IDX[k]]))
        row.append('%6.3f/%5d/%s' % (v, raw, shown))
    print('%2d %s' % (i, ' '.join(row)))
    if i == 0:
        print('   ^ 第 1 次 sample() 恒为 0（没有上一个计数点）')
    sys.stdout.flush()
    time.sleep(1.0)
print('done')
