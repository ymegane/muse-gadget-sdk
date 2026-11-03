# Claude Code への最初の依頼文（コピペ用）

前提：`muse-gadget-sdk` を clone し、`esp32/` ディレクトリで Muse Code を起動した状態で使う。
同じフォルダの `HANDOFF.md` を先に読ませること。

---

このリポジトリの ESP32 Device SDK を、私のボード **Waveshare ESP32-P4-Module-DEV-KIT-B** に対応させたいです。

まず `HANDOFF.md`（別途渡す引き継ぎメモ）と、このリポジトリの `AGENTS.md`、`esp32/devices/waveshare-p4-lcd35.md` を読んでください。出発点は公式サポート済みの Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5（`components/muse/boards/board_waveshare_p4_lcd35.c` と `devices/sdkconfig.muse-waveshare-p4-lcd35`）です。

進め方：
1. 最初に、移植の方針と変更ファイルの一覧だけを提示してください（まだコードは書かない）。
2. 私のボードは 7インチ MIPI-DSI ディスプレイ（720×1280、タッチ GT9271）、ES8311 コーデック（I2S: MCLK13/SCLK12/ASDOUT11/LRCK10/DSDIN9、アンプ有効化 GPIO53）、I2C は SDA=GPIO7/SCL=GPIO8 です。ディスプレイ初期化は Waveshare の `waveshare/esp32_p4_platform` コンポーネントの 7-DSI-TOUCH-A ドライバを参照してください。
3. 不明点（BOOTボタンのGPIO、C6リセットGPIO、P4のシリコンリビジョン）は推測で埋めず、実機確認の手順を私に指示してください。
4. ビルドは ESP-IDF v6.0.1、`esp32p4` ターゲット。まずビルドが通ることを目標にし、書き込みとペアリングはその後で一緒に確認します。
5. SDKトークンは私が menuconfig で入れるので、コードや設定ファイルに直書きしないでください。
