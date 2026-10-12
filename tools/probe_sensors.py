# -*- coding: utf-8 -*-
"""一次性探针：调用 PC 端真实采集逻辑，打印 31 个字段的当前值，
   并标出哪些超过了固件里的告警阈值（超阈值 = 固件会跳页并暂停轮播）。"""
import os
import sys

os.environ['PCMON_NO_HIDE'] = '1'          # 关键：别把测试终端一起藏掉
PC_DIR = r'D:\PC_Monitor\PC端'
sys.path.insert(0, PC_DIR)

import pc_monitor_libre as m               # noqa: E402

# 与 config.h 里的阈值一一对应（单位：°C / %）
THR = {
    'cpu_temp':      ('ALARM_CPU_TEMP', 90.0),
    'gpu_temp':      ('ALARM_GPU_TEMP', 85.0),
    'gpu_hotspot':   ('ALARM_GPU_HOT', 100.0),
    'gpu_mem_temp':  ('ALARM_VRAM_TEMP', 95.0),
    'hdd_temp':      ('ALARM_HDD_TEMP', 55.0),
    'cpu_load':      ('ALARM_CPU_LOAD', 95.0),
    'gpu_load':      ('ALARM_GPU_LOAD', 95.0),
    'mem_used':      ('ALARM_MEM', 90.0),
}

mon = m.LibreHardwareMonitor()
print('initialized      =', mon.initialized)
print('cpu_tctl_tdie    =', mon.cpu_tctl_tdie)
print('cpu_package_power=', mon.cpu_package_power)
print('-' * 62)

data = mon.collect_all_data()
hit = []
for k, v in data.items():
    mark = ''
    if k in THR:
        name, th = THR[k]
        if isinstance(v, (int, float)) and v is not None and v >= th:
            mark = '   <=== 超阈值 %s=%.0f → 固件锁页/告警' % (name, th)
            hit.append((k, v, name, th))
    print('%-16s = %s%s' % (k, v, mark))

print('-' * 62)
if hit:
    for k, v, name, th in hit:
        print('触发告警: %-14s 当前 %s  阈值 %.0f  (%s)' % (k, v, th, name))
    print('→ 温度类告警会把画面锁在第 2 页 THERMAL；负载/内存类会锁在第 1 页 OVERVIEW')
else:
    print('当前没有任何字段超过告警阈值')
