# ============================================================================
#  make_logo.py —— 把开机画面的品牌字渲染成 8 位 alpha 位图（生成 C 头文件）
# ----------------------------------------------------------------------------
#  用法（只需在改品牌字/换字体时跑一次）：
#      <python> -B tools/make_logo.py
#  依赖 Pillow —— 只是**开发期**工具，PC 端运行不需要它。
#
#  为什么不用 TFT_eSPI 的字体直接 drawString：
#    GLCD / FONT2 / FONT4 都只有 ASCII 字形，没有汉字；库的 smooth font 又要额外
#    字库文件。开机画面只需要固定四个字，直接把字形嵌成位图最省事，也最省 flash
#    （只有这四个字，不是一整套字库）。
#
#  为什么是 8 位 alpha 而不是 1 位：
#    32px 的汉字笔画细，1 位（非黑即白）会毛边；8 位 alpha 能在黑底上做抗锯齿。
#    固件侧按 alpha 把品牌色混到背景色上（见 screen_draw.cpp 的 bootDrawLogo()），
#    所以仿真和真机渲染完全一致。
#
#  为什么用超采样：先在 4 倍尺寸上渲染再 LANCZOS 缩小 —— 等于自带超采样，
#  比直接在小尺寸上画要干净得多。
# ============================================================================
import os
import sys

from PIL import Image, ImageDraw, ImageFont

TEXT = '奶娃科技'
TARGET_H = 32                     # 位图目标高度（像素）
SUPER = 4                         # 超采样倍数
FONT_CANDIDATES = [
    r'C:\Windows\Fonts\msyhbd.ttc',   # 微软雅黑 Bold
    r'C:\Windows\Fonts\msyh.ttc',     # 微软雅黑
    r'C:\Windows\Fonts\simhei.ttf',   # 黑体
    r'C:\Windows\Fonts\Deng.ttf',     # 等线
]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_HEADER = os.path.normpath(os.path.join(HERE, '..', 'PC_Monitor', 'boot_logo.h'))
OUT_PREVIEW = os.path.join(HERE, 'logo_preview.png')


def pick_font():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    sys.exit('找不到中文字体（试过：%s）' % '、'.join(FONT_CANDIDATES))


def render_alpha():
    """渲染成 L 模式（灰度=alpha）的位图，并裁到墨迹边界"""
    font_path = pick_font()
    font = ImageFont.truetype(font_path, TARGET_H * SUPER)
    # 先量一下尺寸（CJK 字形基本占满 em 框，这里只是留足画布）
    box = font.getbbox(TEXT)
    W = (box[2] - box[0]) + 20 * SUPER
    H = (box[3] - box[1]) + 20 * SUPER
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text((10 * SUPER - box[0], 10 * SUPER - box[1]), TEXT, fill=255, font=font)

    bbox = img.getbbox()
    if not bbox:
        sys.exit('渲染结果是空图，字体可能没有这几个字：%s' % TEXT)
    img = img.crop(bbox)

    # 缩到目标高度（宽度按比例），LANCZOS 相当于再做一次高质量重采样
    w = max(1, round(img.width * TARGET_H / img.height))
    img = img.resize((w, TARGET_H), Image.LANCZOS)
    return font_path, img


def main():
    font_path, img = render_alpha()
    px = list(img.tobytes())          # L 模式：每像素 1 字节，正好就是 alpha
    W, H = img.size
    ink = sum(1 for v in px if v > 8)
    print('字体  : %s' % font_path)
    print('位图  : %dx%d（墨迹 %d 像素，占 %.0f%%）' % (W, H, ink, 100.0 * ink / (W * H)))
    print('整幅宽 : %dpx（居中后左边距 %d）' % (W, (320 - W) // 2))
    print('数值   : %d 字节（8 位 alpha）' % (W * H))

    with open(OUT_HEADER, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// ============================================================================\n')
        f.write('//  boot_logo.h —— 开机画面的品牌位图（由 tools/make_logo.py 生成，不要手改）\n')
        f.write('// ----------------------------------------------------------------------------\n')
        f.write('//  文字 : %s\n' % TEXT)
        f.write('//  字体 : %s\n' % os.path.basename(font_path))
        f.write('//  规格 : %dx%d，8 位 alpha（0=透明，255=全亮），逐行、左上起\n' % (W, H))
        f.write('//  改字/换字体：改 make_logo.py 里的 TEXT / FONT_CANDIDATES 后重跑本脚本。\n')
        f.write('//  注：保护宏叫 BOOT_LOGO_BITMAP_H，**不能**叫 BOOT_LOGO_H ——\n')
        f.write('//      那会和下面的尺寸常量同名，宏会把常量名替换掉，直接编译失败。\n')
        f.write('// ============================================================================\n')
        f.write('#ifndef BOOT_LOGO_BITMAP_H\n#define BOOT_LOGO_BITMAP_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('static const int BOOT_LOGO_W = %d;\n' % W)
        f.write('static const int BOOT_LOGO_H = %d;\n\n' % H)
        f.write('static const uint8_t BOOT_LOGO_ALPHA[BOOT_LOGO_H * BOOT_LOGO_W] = {\n')
        for j in range(H):
            row = px[j * W:(j + 1) * W]
            f.write('  ' + ','.join('%3d' % v for v in row) + ',   // y=%d\n' % j)
        f.write('};\n\n#endif  // BOOT_LOGO_BITMAP_H\n')
    print('已写出: %s' % OUT_HEADER)

    # 顺带存一张预览图（黑底白字，和屏幕上看到的一致），便于肉眼确认
    prev = Image.new('RGB', (img.width, img.height), (0, 0, 0))
    prev.paste(Image.new('RGB', img.size, (255, 255, 255)), (0, 0), img)
    prev.resize((img.width * 3, img.height * 3), Image.NEAREST).save(OUT_PREVIEW)
    print('预览图: %s' % OUT_PREVIEW)


if __name__ == '__main__':
    main()
