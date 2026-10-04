[English version](README.md)

# DV3000Client

**ThumbDV**(AMBE3000)USBドングルを使用する、デジタル音声無線のリフレクター・
ネットワーク向け Linux デスクトップクライアントです:

- **D-Star**: DExtra プロトコル経由(XLX / XRF 系リフレクター)
- **DMR**: 2つのネットワークに対応 — **BrandMeister**(Open DMR Terminal プロトコル
  経由)と **TGIF**(Homebrew/MMDVM プロトコル経由)

本クライアントにソフトウェア・ボコーダーは含まれていません。AMBEのエンコード/デコード
はすべて ThumbDV ハードウェアがシリアル経由(同梱の `serialDV` ライブラリ経由)で行う
ため、送信・受信のいずれにもドングルが必要です。

メインプログラムはプロトコルごとにタブを持つ Qt6 GUI(`dv3kclient`)です。このリポジ
トリではほかにいくつかのコマンドラインテストツールもビルドされます(下記参照)。

**ダウンロード:** ビルド済みの `.deb` と AppImage は
<https://keit.github.io/DV3000Client/> から、セットアップ手順は
[はじめに](https://keit.github.io/DV3000Client/ja/getting-started.html) にあります。
この README の以降の内容はソースからのビルドについてです。

## 機能

- **D-Star タブ:** リフレクター検索(XLXディレクトリをリアルタイム取得)、モジュール
  選択、接続/切断、PTT送信、直近に受信したヘッダー、Last Heard(受信履歴)一覧。
- **DMR タブ:** ネットワーク選択(BrandMeister または TGIF)、そのネットワーク用の
  トークグループ検索、グループコール(BrandMeister ではプライベートコールも可)、現在
  の購読状況の表示、ネットワークごとのお気に入りリスト、コールサインとトークグループ
  名を解決した Last Heard 一覧。
- **音量:** 両タブ下部にマイク/スピーカーのスライダーとレベルメーターがあり、交信中
  でも調整できます。
- **設定:** コールサイン/DMR ID、DMR ネットワークごとのサーバーとパスワード、オー
  ディオ入出力デバイス(出力用のテスト音、入力用のライブレベルメーター付きテストボタ
  ン)、ThumbDV のシリアルデバイス。
- ディレクトリデータ(リフレクター、トークグループ、DMR ID)はディスクにキャッシュさ
  れ、最短でも24時間ごとに更新されます。

## 動作要件

- Linux(Ubuntu上で開発。Raspberry Pi でも動作実績あり)
- ThumbDV USBドングル(本機に直接接続するか、
  [AMBEServer 3000](https://www.pa7lim.nl/ambeserver-3000-for-linux/) を動かす
  別の機体(Raspberry Piなど)に接続し、UDP経由で利用)
- マイクとスピーカー用の ALSA サウンドデバイス(USBヘッドセットで問題なく動作)
- C++17 対応コンパイラ、CMake 3.17 以上、pkg-config
- ALSA 開発用ヘッダー
- Qt6 Widgets 開発用ファイル(GUIを使う場合のみ必要)

Ubuntu/Debian の場合:

```sh
sudo apt install build-essential cmake pkg-config git libasound2-dev qt6-base-dev
```

Qt6 が見つからない場合、CMake は GUI をスキップしコマンドラインツールのみをビルドします。

## ビルド

このリポジトリは git サブモジュール(`serialDV` と `xlxd`)を使用しているため、
`--recurse-submodules` を付けてクローンしてください。

```sh
git clone --recurse-submodules git@github.com:keit/DV3000Client.git
cd DV3000Client

cmake -S . -B build
cmake --build build -j"$(nproc)"
```

サブモジュールなしで既にクローン済みの場合:

```sh
git submodule update --init --recursive
```

ビルドすると `build/` 以下に次のバイナリが生成されます:

| バイナリ             | 用途                                                                         |
| -------------------- | ---------------------------------------------------------------------------- |
| `dv3kclient`         | Qt GUI 本体(D-Star タブ・DMR タブ)                                           |
| `dextra_test`        | コマンドライン版 DExtra クライアント(ライブ音声モード対応)                   |
| `dmr_test`           | Homebrew/MMDVM マスター(TGIF など)向けコマンドライン版 DMR クライアント      |
| `odt_test`           | コマンドライン版 BrandMeister Open DMR Terminal クライアント(ライブ音声対応) |
| `roundtrip_test`     | ThumbDV を通した PCM→AMBE→PCM のラウンドトリップテスト                       |
| `xlx_directory_test` | XLX リフレクターディレクトリを取得して表示                                   |

## 初回セットアップ

**シリアルデバイスへのアクセス。** ThumbDV はシリアルデバイスとして認識されます。デバ
イスを所有するグループに自分自身を追加し、一度ログアウトしてから再度ログインしてくだ
さい:

```sh
sudo usermod -aG dialout "$USER"
```

固定のデバイスパスを確認します(この値を設定画面に入力します):

```sh
ls -la /dev/serial/by-id
```

**FTDI レイテンシタイマー。** ThumbDV は FTDI チップを使用しており、デフォルトの
16msのレイテンシタイマーが音声のグリッチ(再生のアンダーラン、バックログの増大)を
引き起こします。DV3K Client は同じマシンに接続された ThumbDV を開くたびに自分で
1ms に設定しようとし、それができなかった場合はタブの上に黄色い警告を表示します。
**下の udev ルールが必要なのは、この警告が表示された場合だけです。**
ThumbDV を AMBEServer 経由で使っている場合は、DV3K Client を動かすマシンではなく、
AMBEServer を動かしているマシンにルールを設定してください。恒久的に1msに設定するには:

```sh
echo 'ACTION=="add", SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"' \
  | sudo tee /etc/udev/rules.d/99-ftdi-latency.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

設定後はドングルを一度抜き差ししてください。以下で確認できます:

```sh
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer   # 1 と表示されればOK
```

**Wi-Fi の省電力機能。** Wi-Fi でネットワークに接続している機体では、しばらく通信が
なかった後に通信が始まると(PTT を押した瞬間がまさにこれです)、省電力機能がパケット
を 100ms 以上保留することがあります。ThumbDV を AMBEServer 経由で使う場合、これによ
り送信・受信の音声が途切れるため、両端で省電力機能をオフにしてください。現在の状態は
`iw dev <インターフェース名> get power_save` で確認できます(インターフェース名は
`iw dev` で確認)。

_Raspberry Pi(Raspberry Pi OS、Pi-Star):_ インターフェースはカーネル名の `wlan0`
のままで、NetworkManager も使われていないため、udev ルールで設定できます:

```sh
echo 'ACTION=="add", SUBSYSTEM=="net", KERNEL=="wlan*", RUN+="/sbin/iw dev $env{INTERFACE} set power_save off"' \
  | sudo tee /etc/udev/rules.d/70-wifi-powersave-off.rules
sudo reboot
```

_Ubuntu(および NetworkManager を使うデスクトップ):_ 上記の udev ルールはここでは効
きません。ルールのコマンドが実行される前にインターフェース名が変更され(例: `wlan0`
→ `wlp1s0`)、さらに接続時に NetworkManager が省電力機能を再びオンにするためです
(Ubuntu には `default-wifi-powersave-on.conf` が同梱されています)。代わりに
NetworkManager の設定で上書きします。最後に読み込まれたファイルが優先されるため、ファ
イル名は `default-...` より後に並ぶものにしてください:

```sh
printf '[connection]\nwifi.powersave = 2\n' \
  | sudo tee /etc/NetworkManager/conf.d/wifi-powersave-off.conf
sudo systemctl restart NetworkManager
```

**バックグラウンドの Wi-Fi スキャン。** これは ThumbDV を別機体(AMBEServer)で
使い、この PC から Wi-Fi 経由で接続している場合にのみ関係します。ローカルのシリアル
接続の ThumbDV や有線接続では影響ありません。Wi-Fi カードは数分おきに全チャンネルを
スキャンし、その間約9秒間、無線が接続中のネットワークのチャンネルから離れます。その
間、AMBEServer からの応答はすべて約150ms 遅れ(1フレームの許容時間を大きく超えます)、
受信中は数分おきに約10〜20秒間音声が途切れます。短いテストはスキャンの合間に収まりや
すく、問題なく見えることがあります。GNOME/NetworkManager のデスクトップでは、このス
キャンを起こすものが2つあり(それぞれ約5分ごと)、両方を止める必要があります:

1. _位置情報サービス(geoclue)_。現在地を調べるためにスキャンします。位置情報サー
   ビスをオフにしてください(設定 > プライバシーとセキュリティ > 位置情報サービス、
   または下記のコマンド)。この設定はユーザーごとに保存され、再起動後も有効です。代
   わりに位置情報を使う機能は使えなくなります(例: 夜間モードの「日没から日の出まで」
   の自動スケジュール。手動スケジュールに切り替えてください)。

   ```sh
   gsettings set org.gnome.system.location enabled false
   ```

2. _NetworkManager のローミング用スキャン_。より良いアクセスポイントを探すためのも
   のです。接続を現在使っているアクセスポイント(BSSID)に固定すると止まります。代わ
   りに PC が自動で別のアクセスポイントに移らなくなります。ルーター1台のデスクトップ
   なら問題ありませんが、メッシュネットワークや持ち歩くノート PC では、アクセスポイン
   トが変わったときに固定を解除する(`802-11-wireless.bssid ""`)か、新しい BSSID を設
   定し直す必要があります。`MyWiFi` は自分の接続名に置き換えてください
   (`nmcli connection show` で確認):

   ```sh
   iw dev wlp1s0 link | head -1    # "Connected to xx:xx:xx:xx:xx:xx"
   nmcli connection modify MyWiFi 802-11-wireless.bssid xx:xx:xx:xx:xx:xx
   nmcli connection up MyWiFi
   ```

全チャンネルのスキャンが止まったか確認するには、数分間監視します。全チャンネルのスキャ
ンでは `scan finished` の行に多数の周波数が並びます。自分のチャンネル(周波数1つ)だ
けの短いスキャンは問題ありません。

```sh
iw event -t | grep scan
```

## アプリケーションの起動

```sh
./build/dv3kclient
```

デスクトップのアプリケーションランチャー(GNOME、KDE など)から起動したい場合は、
次のスクリプトで現在のユーザー用にアイコン付きのランチャー項目を追加できます。この
リポジトリの `build/dv3kclient` を指すため、リポジトリを移動した場合は再実行してくださ
い。`--uninstall` で削除できます。

```sh
./scripts/install-desktop-entry.sh
```

1. **ファイル > 設定...** を開き、以下を入力します:
   - **General(一般):** D-Star のコールサインとモジュール文字。DMR用にはDMR ID、
     続いてネットワークごとの項目。_DMR network_ ボックスで **BrandMeister** または
     **TGIF** を選び、それぞれの項目を表示・編集します。両方入力しても構いません —
     サーバーとパスワードが設定されているネットワークのみがDMRタブに表示されます。
     TGIF にはさらに ID サフィックスと位置情報/説明/URLの項目があります。(周波数・
     カラーコード・タイムスロットはRF専用のためソースコード側に固定値として埋め込ま
     れており、設定項目にはありません。)
   - **Devices(デバイス):** オーディオの入力・出力デバイス(**Test** ボタンで正しい
     デバイスを選べているか確認できます)と ThumbDV(ローカルのシリアルデバイス、また
     はネットワークのホストとポート — 詳細は下記)。
2. **D-Star タブ:** リフレクターを選び、対象モジュールを選択して **Connect** をク
   リックします。
3. **DMR タブ:** (切断中に)**Network** を選び、**Connect** をクリック、トークグ
   ループを選択または入力し、**PTT to send** を使用します。
4. **PTT to send** は送信のON/OFFを切り替えます(クリックするか、テキスト欄にフォー
   カスがない状態でスペースキーを押します)。

接続中は設定を変更できません。

### 別システム上の ThumbDV(AMBEServer 3000)

ローカルのシリアルデバイスの代わりに、ドングルを Raspberry Pi(または任意の Linux
機)に接続し、PA7LIM 氏の AMBEServer 3000 を動かして UDP にブリッジすることもできま
す。**設定 > Devices** で ThumbDV を **Network (AMBEServer 3000)** に設定し、ホスト
名または IPv4 アドレスとポート(デフォルト 2460)を入力、**Test** を押してサーバー
から応答があるか確認してください。

- AMBEServer に同時に接続できるクライアントは1つだけです。GUIクライアントとコマンドラインツール
  を同じサーバーに対して同時に実行しないでください。
- 20msごとの音声フレームはそれぞれUDPのラウンドトリップになるため、有線LANが最適で
  す。Wi-Fiを使う場合は両端で省電力機能をオフにし、この PC のバックグラウンドスキャン
  も止めてください([初回セットアップ](#初回セットアップ)の _Wi-Fi の省電力機能_ と
  _バックグラウンドの Wi-Fi スキャン_ を参照)。応答が100msを超えた場合はロストとして扱
  われます。
- コマンドラインツールもシリアルデバイスの代わりに同じ形式を指定できます。例:
  `./build/roundtrip_test 192.168.1.20:2460 in.raw out.raw`
- **Raspberry Pi の USB ストール。** Pi 3B+ では ThumbDV の USB 接続がときどきストー
  ルし、Pi に
  `ftdi_sio ttyUSB0: usb_serial_generic_read_bulk_callback - urb stopped: -32`
  と記録されます(`dmesg -T | grep "urb stopped"` で確認)。AMBEServer はポートを開き
  直さないため、以後まったく応答しなくなります。GUIクライアントのログは
  `getResponse: cannot get response` で埋まり、AMBEServer を再起動するまで何も聞こえ
  ません。`scripts/pi-star/` にはカーネルログを監視し、このストールが出たら
  AMBEServer を再起動する小さなウォッチドッグサービスがあります。これにより失うのは1〜
  2秒の音声だけになります。このディレクトリを Pi にコピーして次を実行してください:

  ```sh
  sudo ./install-ambeserver-watchdog.sh
  ```

  サービス名が `ambeserver` であることを前提としています(異なる場合はユニットファイ
  ルで `AMBESERVER_SERVICE` を設定してください)。Pi-Star の読み取り専用ルートファイル
  システムはインストール中だけ書き込み可能にします。再起動の記録は
  `journalctl -u ambeserver-watchdog` で確認できます。

### DMR に関する補足

- DMR ID は登録済みのIDです。各ネットワークはそれぞれ独自の認証情報を必要とします —
  BrandMeister は SelfCare で設定した **Hotspot Security** パスワード(アカウントの
  パスワードではありません)、TGIF は TGIF アカウントのセキュリティページで生成される
  **Hotspot Security Key**(16桁)。
- サーバー欄はホスト名のみを入力します。ポートはネットワークごとに固定(TGIF の
  Homebrew は 62031、BrandMeister の Open DMR Terminal は 54006)なので追加しないで
  ください。BrandMeister のサーバー欄は BrandMeister から取得しキャッシュした(1日
  有効)マスター一覧のドロップダウン(「AU 5051」「DE 2621」など)です。
  `3101.master.brandmeister.network` のようにホスト名を直接入力することもできます。
- **BrandMeister(Open DMR Terminal)** は購読(subscribe)しているトークグループの
  音声のみを受信します。欄のトークグループは接続時ではなく、PTT を押したときに購読し
  ます。**Unsubscribe**(_Current subscription_ の隣)で購読を解除でき、切断時には自動
  的に購読解除されます。**プライベートコール**(例: Parrot エコーテスト、ID 9990)を
  行う場合は **Private call** にチェックを入れてください。この場合トークグループ欄は
  宛先のDMR IDになります。
- **TGIF(Homebrew)** には購読という概念がなく、最後に送信したトークグループの音声
  が聞こえます。これが _Current subscription_ に表示される内容です。TG 4000 はトラ
  フィックを流さない「待機場所」で、「None」と表示されます。TGIF はプライベートコー
  ルに対応していないためチェックボックスは無効になっています。テストには TG 9990 ま
  たは 31000(Parrot)への **グループコール** を使用してください。
- **DMR ID サフィックス(TGIF)**: 同じ DMR ID の別のホットスポットと並行してこのク
  ライアントを動かす場合は、2桁のサフィックスを入力してください。これが付加されて一
  意な9桁のID(TGIFの「ESSID」)になります。空欄のままにするとそのままの7桁のIDが
  使われます。
- トークグループ番号の意味はネットワークごとに異なるため、トークグループ一覧とお気
  に入りはネットワークごとに個別に管理されます。

### GUIクライアント が書き込むファイル

`~/.config/DV3000Client/` 以下に格納されます:

| ファイル                   | 内容                                                          |
| -------------------------- | ------------------------------------------------------------- |
| `settings.json`            | 設定内容(DMRホットスポットのパスワードを平文で含みます)       |
| `dmr_favourites.json`      | BrandMeister のお気に入りリスト                               |
| `dstar_favourites.json`    | D-Star のお気に入り(リフレクター + モジュール)                |
| `dmr_favourites_tgif.json` | TGIF のお気に入りリスト                                       |
| `dv3000client.log`         | ログファイル(**Help > Log File Location...** からも開けます)  |
| `cache/`                   | リフレクター・トークグループ・DMR ID ディレクトリのキャッシュ |

## コマンドラインツール

使用例です。引数なしで実行すると使い方が表示されます。

```sh
# D-Star: モジュールBでリフレクターにリンクし、オーディオデバイスでライブ動作させる
./build/dextra_test <reflector-ip> B /dev/serial/by-id/<thumbdv> live plughw:1,0 plughw:1,0

# DMR(Homebrew、例: TGIF): ログインし Parrot(TG 9990へのグループコール)へ短い
# テスト送信を行う。20秒間接続を維持する。パスワードに "-" を指定すると
# $DMR_PASSWORD から読み込むため、シェル履歴には残らない。
read -s "DMR_PASSWORD?TGIF Hotspot Security Key: "; export DMR_PASSWORD
./build/dmr_test --suffix 01 tgif.network 62031 <dmrId> - <callsign> 20 9990

# DMR(BrandMeister Open DMR Terminal): 実際のオーディオで接続し、TG 9990(パロット TG)に接続、エコーテストを行う。
./build/odt_test <master-host> <dmrId> "$PASSWORD" /dev/serial/by-id/<thumbdv> plughw:1,0 plughw:0,0 9990 private
```

```sh
# BrandMeisterのマスターサーバーのリストは以下のURLを参照
https://brandmeister.network/#/masters
```

```sh
# aplay & arecordを使って、ALSAデバイス・ネームを表示できます。
aplay -l
arecord -l
```

### ローカルテスト用リフレクター

パブリックネットワークに触れずに D-Star をテストするには、同梱のサブモジュールから
ローカルの `xlxd` リフレクターをビルド・実行します:

```sh
./scripts/run-test-xlxd.sh XLX999 127.0.0.1     # 別の機体からアクセスする場合は 0.0.0.0 で待ち受ける
```

`xlxd` は無改変のまま同梱されており、ビルド時にスクリプトがローカルテスト用の小さな
パッチ(`scripts/xlxd-local-test.patch`)を適用します。

## プロジェクト構成

```
src/                  各プロトコルのクライアント、オーディオ、DMR音声/FEC、CLIツール
src/gui/              Qt GUI(メインウィンドウ、D-Starタブ、DMRタブ、設定)
data/                 リフレクター一覧のフォールバック用静的データ
resources/            アプリケーションアイコン(1024px の原画から生成)と Qt リソースファイル
scripts/              ローカル xlxd テストリフレクター用スクリプト、ランチャー登録
scripts/pi-star/      Pi-Star / Raspberry Pi 用の AMBEServer ウォッチドッグ
third_party/serialDV  ThumbDV/AMBE3000用シリアルドライバー(フォーク、サブモジュール)
third_party/xlxd      xlxd のリファレンスコード(サブモジュール。DMRのFECコードを再利用)
```

## 謝辞

- [serialDV](https://github.com/f4exb/serialDV)(F4EXB 氏作) — ThumbDV のシリアル
  インターフェース(本リポジトリではフォーク版を使用)
- [xlxd](https://github.com/LX3JL/xlxd)(LX3JL 氏作) — D-Star/DMR 関連コードはプロ
  トコル実装の参考にし、FECルーチンは再利用しています

## ライセンス

本プロジェクトは GNU General Public License v3.0(GPLv3)のもとで公開されています。
詳細は [LICENSE](LICENSE) ファイルを参照してください。`serialDV`(GPLv3)のコードと、
`xlxd` の DMR FEC エンコーダー(GPLv2 以降)を含んでいます。xlxd の GPLv2 限定の
`cutils` ヘルパーは使用せず、`src/dmr_fec` に独自の実装を用意しています。
