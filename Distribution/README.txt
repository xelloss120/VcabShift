VcabShift 1.0.0
===============

声の高さ（ピッチ）と声色（フォルマント）を変えるボイスチェンジャーです（TD-PSOLA）。
対応: Windows（64bit）

■ 中身
  VcabShift.exe   単体で動く EXE 版
  VcabShift.vst3  VST3 プラグイン

■ 使い方
  EXE 版: VcabShift.exe を実行し、「オーディオ設定」ボタンで
          入力と出力のデバイスを選びます。
  VST3 版: VcabShift.vst3 をフォルダごと
          C:\Program Files\Common Files\VST3 にコピーし、DAW で読み込みます。

■ 注意
  ・ヘッドフォンで使ってください。スピーカーだとハウリング（大きな音）が
    起きることがあります。EXE 版は起動してすぐ音が出ます。
  ・遅延は「探索下限」で決まります（80Hz で 25ms）。自分の声の低さに
    合わせて上げると、遅延が減ります。
  ・署名をしていないため、初回に「PC が保護されました」と出たら
    「詳細情報」→「実行」を押してください。
  ・EXE 版の設定は %APPDATA%\VcabShift に保存されます。

■ 画面
  右のグラフは、入力（青）と出力（橙）のスペクトルと、周波数ごとに
  フォルマントのつまみからさらに動かす量（桃の線。0 はつまみのとおり）です。
  F1〜F4 の点は、ドラッグで動かせます。
  詳しい説明は、つまみや項目にマウスを乗せると出ます。

■ ライセンス
  Copyright (C) 2026 x120
  AGPLv3（LICENSE.txt）。使っているライブラリのライセンスは
  THIRD_PARTY_LICENSES.md にあります（GPLv3 の本文は GPL-3.0.txt）。
  この版のソースコード:
    https://github.com/xelloss120/VcabShift/releases/tag/v1.0.0
  VST is a trademark of Steinberg Media Technologies GmbH.
