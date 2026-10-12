#ifndef SCREEN_DRAW_H
#define SCREEN_DRAW_H

#include <TFT_eSPI.h>
#include "config.h"

extern TFT_eSPI tft;

void screenInit();      // 初始化屏幕 + 建立离屏缓冲 + 画静态界面
void drawStaticUI();    // 只画不随数据变化的部分（卡片框、标签、图例、刻度）
void drawMainUI();      // 每帧调用：按行增量刷新数值、波形、告警、链路状态

#endif
