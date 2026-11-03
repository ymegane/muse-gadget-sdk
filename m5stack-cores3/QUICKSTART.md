# M5Stack CoreS3 クイックスタート（Muse Gadget SDK）

M5Stack CoreS3（K128）は Muse Gadget SDK の**公式サポート対象**なので、移植なしでそのまま動く。
技適マークは筐体側面に表示あり（スイッチサイエンス販売分で確認）。2026-11-03 に購入。

## 前提

- ESP-IDF **v6.0.1**（他のバージョン不可）
- SDKトークン：https://gadgets.muse.ai/settings/sdk-tokens （Account > SDK tokens）で発行
  - **トークンはこのリポジトリに絶対コミットしない**
- Museアプリの Developer mode（Settings > Devices）

## 手順

```sh
git clone https://github.com/ymegane/muse-gadget-sdk.git
cd muse-gadget-sdk/esp32

# ESP-IDF v6.0.1 の環境を有効化してから
tools/muse/board.sh build cores3
tools/muse/board.sh flash cores3
```

- トークン設定：`idf.py menuconfig` → 「ESP32 Device SDK > Muse Gadgets SDK token」
  - 後からシリアルで送ることもできる：`>sdk_token=<token>` と送ると保存されて再起動する
- あとは画面の案内に従ってペアリング（確定操作は PWR ボタン）

## CoreS3 固有のメモ（SDKの devices/README より）

- ユーザーボタンは無し。左側面の **PWR が push-to-talk 兼ペアリング確定**
- 設定はタッチ画面（Muse の画面から左スワイプ）
- スピーカーは 1W で音量小さめ。SDK側で初期音量 100%
- esptool が繋がらないときは RST を3秒長押し（緑LED点灯）でブートローダに入る
- カメラ・近接センサ・IMU・RTC・SDカード・Groveポートは SDK では未使用
- バッテリー内蔵 500mAh。Muse 実行中に PWR 長押ししても電源は切れない（トーク扱い）
