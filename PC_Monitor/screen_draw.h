#ifndef SCREEN_DRAW_H
#define SCREEN_DRAW_H

#include <TFT_eSPI.h>
#include "config.h"

extern TFT_eSPI tft;

void screenInit();      // 初始化屏幕 + 建立离屏缓冲 + 画开机画面
void drawStaticUI();    // 只画不随数据变化的部分（卡片框、标签、图例、刻度）
void drawMainUI();      // 每帧调用：启动画面期间画动画，连上后按行增量刷新数据
bool uiBooting();       // 还在启动画面（没连上 PC / 没收到数据）
uint16_t uiFrameMs();   // 本阶段建议的帧间隔（启动画面 30fps，数据页 5Hz）

#endif
