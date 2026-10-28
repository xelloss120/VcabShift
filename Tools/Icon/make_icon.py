# アプリのアイコン（Resources/Icon.png と Resources/IconSmall.png）を作る。
# 図柄: VoiChanAlgoBox のアイコン（Resources/IconBase.png）の箱の上に帯を重ね、白で「SHIFT」と書く。
# 帯は箱の枠の外側（上と左右）にそろえる（VcabDenoise と同じ形）。帯の色は、画面のフォルマントの動く量の線と同じ桃色。
# 文字は Noto Sans Bold（SIL Open Font License。Windows に入っている C:\Windows\Fonts\NotoSans-Bold.ttf）。
# 必要なもの: Python 3、Pillow。
# 使い方: python make_icon.py [出力先のフォルダ] [帯の色] [Noto Sans Bold のファイル]
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', '..', 'Resources')
OUT = sys.argv[1] if len(sys.argv) > 1 else RES
BAND = sys.argv[2] if len(sys.argv) > 2 else '#e870a8'
FONT = sys.argv[3] if len(sys.argv) > 3 else r'C:\Windows\Fonts\NotoSans-Bold.ttf'

# 帯の位置（1024 × 1024 の画像の上で、箱の枠の外側）
BOX_LEFT, BOX_RIGHT, BAND_TOP, BAND_BOTTOM, RADIUS = 175, 849, 175, 338, 72
SCALE = 4  # 縁をなめらかにするため、4 倍の大きさで描いてから縮める


def make(size):
    base = Image.open(os.path.join(RES, 'IconBase.png')).convert('RGBA').resize((1024 * SCALE, 1024 * SCALE), Image.LANCZOS)
    layer = Image.new('RGBA', base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.rounded_rectangle([BOX_LEFT * SCALE, BAND_TOP * SCALE, BOX_RIGHT * SCALE, BAND_BOTTOM * SCALE],
                        radius=RADIUS * SCALE, fill=BAND)
    font = ImageFont.truetype(FONT, 118 * SCALE)
    text = 'SHIFT'
    left, top, right, bottom = d.textbbox((0, 0), text, font=font)
    cx = (BOX_LEFT + BOX_RIGHT) / 2 * SCALE
    cy = (BAND_TOP + BAND_BOTTOM) / 2 * SCALE
    d.text((cx - (left + right) / 2, cy - (top + bottom) / 2), text, font=font, fill='white')
    return Image.alpha_composite(base, layer).resize((size, size), Image.LANCZOS)


os.makedirs(OUT, exist_ok=True)
make(1024).save(os.path.join(OUT, 'Icon.png'))
make(256).save(os.path.join(OUT, 'IconSmall.png'))
