# M5Stack CoreS3 クイックスタート（Muse Gadget SDK）

M5Stack CoreS3（K128）は Muse Gadget SDK の**公式サポート対象**なので、移植なしでそのまま動く。
技適マークは筐体側面に表示あり（スイッチサイエンス販売分で確認）。

ここでは Apple Silicon の Mac で、ESP-IDF をホストに入れずに **Apple container でビルド**し、
**書き込みだけ Mac 側の esptool で行う**手順をまとめる。

## 前提

- ESP-IDF **v6.0.3**（フォーク元の CI と同じ。ビルドは公式イメージ `espressif/idf:v6.0.3` を使う）
- [Apple container](https://github.com/apple/container/releases)（1.7.1 で確認）
- Homebrew、pipx、esptool（`brew install pipx && pipx install esptool`、v5.6.0 で確認）
- SDKトークン：https://gadgets.muse.ai/settings/sdk-tokens （Account > SDK tokens）で発行
  - **トークンはこのリポジトリに絶対コミットしない。ログやスクリーンショットにも載せない**
- Museアプリの Developer mode（Settings > Devices）
- 空きディスク 20GB 程度（イメージ展開後 約15GB ＋ ビルド生成物）

## 1. コンテナの準備（初回のみ）

```sh
container system start
container system kernel set --recommended   # 初回と、container を更新したあと

# --arch を付けないと amd64 版まで取得・展開され、約15GB 余計に使う
container image pull --arch arm64 espressif/idf:v6.0.3
```

`~/.zshrc` に `export CONTAINER_DEFAULT_PLATFORM=linux/arm64` を入れておくと、付け忘れを防げる。

## 2. ビルド

リポジトリのルートで実行する（M2/M3 クラスで約140秒）。

```sh
container run --rm -m 8G -c 6 \
  -v "$PWD":/project -w /project/esp32 \
  espressif/idf:v6.0.3 tools/muse/board.sh build cores3
```

生成物は `esp32/build-muse-m5stack-cores3/`（gitignore 済み）。
SDKトークンなしでもビルドは通る（警告が出るだけ）。トークンは書き込み後にシリアルで設定する。

> コンテナからは USB が見えないので、`board.sh flash` はコンテナ内では使えない。

## 3. 出荷時ファームのバックアップ（任意・推奨）

書き込むと出荷時ファームは上書きされる。先に 16MB 全体を読み出しておく（約70秒）。

```sh
esptool --port /dev/cu.usbmodem<番号> read-flash 0 ALL cores3-factory.bin
# 戻すとき
esptool --port /dev/cu.usbmodem<番号> write-flash 0 cores3-factory.bin
```

バックアップはリポジトリの外に保存する。2回読んで `shasum -a 256` が一致すれば確実。

## 4. 書き込み（Mac 側の esptool）

```sh
cd esp32/build-muse-m5stack-cores3
esptool --chip esp32s3 -p /dev/cu.usbmodem<番号> -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

- ポートは `ls /dev/cu.usbmodem*` で確認。出ないときは充電専用ケーブルを疑う
- esptool が繋がらないときは RST を3秒長押し（緑LED点灯）でブートローダに入る
- アプリは開発用の鍵で署名されるが secure boot は無効の設定なので、eFuse は焼かれない

## 5. SDKトークンの設定

シリアルに `>sdk_token=<token>` を1行送ると、ボードが `@sdk_token ok` と返して再起動する。
ボードのログにはトークンの先頭が出るので、ログを貼るときは注意。

[`tools/send_sdk_token.py`](tools/send_sdk_token.py) を使うと、トークンを画面に出さずに送れる
（pyserial が必要。esptool の pipx 環境の python なら入っている）。

```sh
# 1Password CLI から読む場合（参照先は自分の保管庫に合わせる）
MUSE_SDK_TOKEN_REF="op://<vault>/<item>/credential" \
  "$HOME/Library/Application Support/pipx/venvs/esptool/bin/python" -I \
  m5stack-cores3/tools/send_sdk_token.py /dev/cu.usbmodem<番号>

# 手で貼る場合（入力は表示されない）
read -rs "MUSE_SDK_TOKEN?SDK token: " && echo && export MUSE_SDK_TOKEN && \
  "$HOME/Library/Application Support/pipx/venvs/esptool/bin/python" -I \
  m5stack-cores3/tools/send_sdk_token.py /dev/cu.usbmodem<番号>; unset MUSE_SDK_TOKEN
```

`menuconfig`（「ESP32 Device SDK > Muse Gadgets SDK token」）でビルドに埋め込む方法もあるが、
コンテナでのビルドと相性が悪く、生成物にトークンが残るのでおすすめしない。

> 1Password CLI がターミナルによっては「No accounts configured」になることがある。
> アプリ連携（設定 > 開発者 > CLI と連携）をオンにし、ターミナルアプリを開き直すと直ることが多い。

## 6. ペアリング

1. Muse アプリ（Developer mode）で `MuseGadget-XXXXXX` を選ぶ
2. 本体左側面の **PWR** で確定
3. Wi-Fi は **2.4GHz の SSID** を選ぶ（ESP32-S3 は 5GHz 非対応）
4. 設定が終わると自動で再起動する

ペアリング中は BLE と Wi-Fi を同時に使うため、IP 取得まで30秒近くかかることがある。少し待つ。

### ファームを入れ替えてコマンドが増減したとき

ボードは接続のたびに新しいコマンド一覧を送るが、Muse 側は前の一覧を使い続けることがある。
Muse に「このデバイスで使えるコマンドを一覧で教えて」と聞いて、古い一覧のままなら、
なくなったコマンド（例：`sensors.read`）を 1 回呼ばせると取り直された（2026-11 時点の観察）。
だめなら Muse アプリでデバイスを削除して再ペアリングする。

## CoreS3 固有のメモ（SDKの devices/README より）

- ユーザーボタンは無し。左側面の **PWR が push-to-talk 兼ペアリング確定**
- 設定はタッチ画面（Muse の画面から左スワイプ）
- スピーカーは 1W で音量小さめ。SDK側で初期音量 100%
- 照度センサー（LTR-553）は `sensors.read`、前面カメラ（GC0308）は `camera.capture`（320x240 の JPEG）で使える。近接センサ・IMU・RTC・SDカード・Groveポートは SDK では未使用
- カメラは撮影のときだけ起動する（1回 約2秒）。Muse には画像が base64 で届く
- 日本語の字幕はプロファイルで `CONFIG_MUSE_CJK_FONT=y` にして表示している
- バッテリー内蔵 500mAh。Muse 実行中に PWR 長押ししても電源は切れない（トーク扱い）
