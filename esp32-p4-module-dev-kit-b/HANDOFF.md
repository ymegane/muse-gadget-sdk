# 引き継ぎメモ：Muse Gadget SDK を Waveshare ESP32-P4-Module-DEV-KIT-B に載せる

作成：2026-11-03（Muse / ふわ が調査・整理）。最終作業はユーザーのPC上で Claude Code 等が実施する前提。

## ゴール

Meta のオープンソース **Muse Gadget SDK**（`facebookincubator/muse-gadget-sdk`、Apache 2.0）の ESP32 Device SDK を、
手持ちの **Waveshare ESP32-P4-Module-DEV-KIT-B** に移植し、Muse アプリとペアリングできる音声ガジェットにする。

完了の定義：
1. `esp32p4` ターゲットでビルドが通る
2. ボードに書き込みでき、シリアルログで起動が確認できる
3. Muse アプリ（設定 > デバイス > 開発者モード ON）に `MuseGadget-XXXXXX` として現れ、ペアリングできる
4. 画面にUIが出て、ボタン長押し（プッシュ・トゥ・トーク）→ 音声で応答が返る

## 対象ボードの仕様（確認済み）

| 項目 | 内容 |
|---|---|
| 製品 | Waveshare ESP32-P4-Module-DEV-KIT **B**（SKU系：DEV-KIT-B） |
| SoC | ESP32-P4NRW32X（デュアルコア RISC-V）＋ 無線用 ESP32-C6（モジュール内蔵、SDIO接続） |
| メモリ | PSRAM 32MB、Nor Flash 16MB |
| ディスプレイ | 7" DSI タッチディスプレイ同梱（7-DSI-TOUCH-A）：720×1280、MIPI-DSI 2-lane、タッチ GT9271（I2C 0x5D または 0x14）。パネル駆動ICは ILI9881C 系（Waveshare の esp32_p4_platform に 7-DSI-TOUCH-A 用ドライバあり。要・実機確認） |
| 音声 | コーデック **ES8311**（I2C 0x18）＋ アンプ NS4150B、オンボードマイク（ES8311 ADC入力）、スピーカーは MX1.25 2P ヘッダー（8Ω 2W） |
| I2S ピン | MCLK=GPIO13、SCLK=GPIO12、ASDOUT=GPIO11、LRCK=GPIO10、DSDIN=GPIO9、アンプ有効化=GPIO53（Active High） |
| I2C バス | SDA=GPIO7 / SCL=GPIO8 を ES8311・タッチ・カメラSCCB で共有 |
| その他 | RJ45 Ethernet、USB Type-C（UART/書込み）×2系統、BOOT / RST ボタン、TFスロット、40pin GPIO |

出典：Waveshare 製品ページ、esp-cpp/espp の BSP、コミュニティの回路図起こし（Erablitek/waveshare-esp32-p4-devkit-b）。**GPIO とパネルICは最終的に公式回路図と実機で再確認すること。**

## SDK 側の出発点（確認済み）

- 同じ P4＋C6 構成の **Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5** が公式サポート済み。これが移植のベースになる。
  - ボード実装：`esp32/components/muse/boards/board_waveshare_p4_lcd35.c`（Waveshare BSP ベース）
  - 設定：`esp32/devices/sdkconfig.muse-waveshare-p4-lcd35`、ボード解説：`esp32/devices/waveshare-p4-lcd35.md`
  - ビルド：`tools/muse/board.sh build p4lcd35`（`tools/muse/board.sh` の case に `p4lcd35) profile=waveshare-p4-lcd35; target=esp32p4`）
  - ベース側の構成：320×480 ST7796 **SPI** LCD、FT6336 タッチ、ES8311（マイク・スピーカー共用）、AXP2101 PMIC、BOOT=GPIO35 がプッシュ・トゥ・トーク
- ESP-IDF は **v6.0.1 固定**（他バージョン不可）。`esp32p4` ツールを入れること。
- PSRAM 32MB あるので、家ネットワーク連携（home-network tunnel）も有効化できる構成。
- 注意：P4 の ROM には JPEG デコーダがないため、Muse が送る画像表示（`display.draw_url`）はベースボード同様オフが無難。

## 移植でやること

1. **新規ボード追加**：`board_waveshare_p4_lcd35.c` をコピーして `board_waveshare_p4_module_dev_kit_b.c`（仮称）を作り、`devices/sdkconfig.muse-waveshare-p4-module-dev-kit-b` と `tools/muse/board.sh` の case を追加する。リポジトリの `AGENTS.md` と既存ボードの追加パターンに従う。
2. **ディスプレイ（最大の作業）**：SPI の ST7796 から **MIPI-DSI** に変更。7-DSI-TOUCH-A（720×1280）の初期化は Waveshare の `waveshare/esp32_p4_platform` コンポーネント（7-DSI-TOUCH-A ドライバ同梱）を参照。パネル電源・バックライトは FFC 経由の PMIC/ブリッジが握っているので、Waveshare のサンプルの初期化シーケンスをそのまま使う。解像度とUIレイアウト（縦 720×1280）を合わせる。
3. **タッチ**：FT6336 → **GT9271**（I2C 0x5D/0x14、GPIO7/8 の共有バス上）。
4. **音声**：コーデックは同じ **ES8311** なので流用しやすい。I2S ピンを上表に差し替え、アンプ有効化 GPIO53 の制御を入れる。マイクはオンボード（ES8311 ADC）。
5. **ボタン**：プッシュ・トゥ・トーク用に DEV-KIT の BOOT ボタンの GPIO を回路図で確認して割り当てる（ベースは GPIO35。本ボードの BOOT の GPIO 番号は**未確認**）。
6. **C6 スレーブ**：ホスト側は ESP-Hosted 2.12 前提。出荷時の C6 ファームが古いと BLE ペアリングが失敗する（ベースボードで実績あり）。必要なら ESP-Hosted の `host_performs_slave_ota` で 2.12 系スレーブ（例：esp32c6-v2.12.13.bin）に更新する。SDIO リセット用 GPIO も本ボードの配線に合わせる（ベースは GPIO54、**要確認**）。
7. **P4 シリコンリビジョン**：v3 系か v1 系かで sdkconfig が変わる（v1 系は `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` 等が必要、ベースの解説参照）。`esptool chip-id` と起動ログで実機確認してから確定する。

## 作業手順（ユーザーのPCで実施）

1. SDK トークンを発行：https://gadgets.muse.ai （Account > SDK tokens、`mgst_` で始まる）。**トークンはこのメモに書かない・コミットしない。**
2. リポジトリ取得：`git clone https://github.com/facebookincubator/muse-gadget-sdk` → `esp32/` で作業。
3. ESP-IDF v6.0.1 を用意（`esp32p4` 含む）。Muse Code を使う場合は README の手順どおり `muse --disable-sandbox` で起動し、AGENTS.md を読ませる。
4. トークン設定：`idf.py menuconfig`（ESP32 Device SDK > Muse Gadgets SDK token）または書き込み後にシリアルで `>sdk_token=...` を送る方法でも可。
5. ビルド → 書き込み → シリアルモニタでログ確認（ポートは macOS なら `/dev/cu.usb*`、Linux なら `/dev/ttyACM*` 等）。
6. ペアリング：Muse アプリで 設定 > デバイス > 開発者モード ON → デバイス追加（＋）→ `MuseGadget-XXXXXX` を選択 → デバイス側のボタン押下で確認。
7. 信頼できる自宅ネットワークでセットアップする（ペアリングにメーカー検証はない、という SDK 側の注意あり）。

## 未確認・リスク

- 7" パネルの駆動IC（ILI9881C とみられるが DEV-KIT-B 同梱パネルでの確定情報なし）→ Waveshare の DEV-KIT 用サンプル/BSP で確認。
- BOOT ボタンの GPIO 番号、C6 リセット GPIO、コンソール UART のチップ（CH343 かどうか）→ 回路図と実機で確認。
- P4 のシリコンリビジョン（v1/v3）→ ビルド設定に影響。
- ディスプレイの PMIC 初期化が SDK の BSP 構造に素直に乗るかは、Waveshare サンプルの作り次第。難しければ「画面なし（ステータス表示のみ）」で先にペアリングまで通し、画面は後付けにする段階分けも可。

## 参考リンク

- SDK リポジトリ：https://github.com/facebookincubator/muse-gadget-sdk （`esp32/README.md`、`esp32/AGENTS.md`）
- ベースボード解説：リポジトリ内 `esp32/devices/waveshare-p4-lcd35.md`
- 製品ページ：https://www.waveshare.com/esp32-p4-module-dev-kit.htm
- BSP（esp-cpp/espp）：https://github.com/esp-cpp/espp （components/esp32-p4-module-dev-kit）
- ESP Component Registry：`waveshare/esp32_p4_platform`（7-DSI-TOUCH-A ドライバ含む）
