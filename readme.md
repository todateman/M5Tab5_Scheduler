# M5Tab5 スケジューラ表示システム

## 概要

M5Stack Tab5（ESP32-P4）上で、SDカードからCSV形式のスケジュールを読み込み、現在時刻・進行中・今後の予定を大画面に表示するシステムです。  
Wi-Fi（WiFiManager）でNTPサーバと自動時刻同期し、RTC（RX8130CE）に書き込むことで、電源を切っても正確な時刻を保持します。

---

## 特徴

- SDカードのCSVファイルでスケジュールを管理
- **WiFiManager** によるキャプティブポータル経由のWi-Fi設定（初回のみ）
- **NTPサーバ（ntp.nict.jp）** との自動時刻同期（JST対応）
- Tab5内蔵のRTC（RX8130CE）で電源断後も時刻保持
- 進行中・今後の予定をそれぞれ最大3・4件表示
- シリアル経由でRTC時刻を手動設定可能

---

## 必要なハードウェア

- M5Stack Tab5
- microSDカード（schedule.csv 配置用）

---

## 必要なライブラリ（PlatformIO自動インストール）

| ライブラリ | 用途 |
| --------- | --- |
| [M5Unified](https://github.com/m5stack/M5Unified) | 画面・ハードウェア制御 |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | Wi-Fi設定ポータル |
| [CSV_Parser](https://github.com/michalmonday/CSV-Parser-for-Arduino/) | CSVファイル解析 |
| [ArtronShop_RX8130CE](https://github.com/ArtronShop/ArtronShop_RX8130CE) | RTC制御 |

---

## セットアップ手順

1. **SDカードにスケジュールCSVを配置**

   ファイル名：`/schedule.csv`

   ```csv
   start,stop,action
   2025/6/8 9:00,2025/6/8 10:00,会議
   2025/6/8 14:00,2025/6/8 15:00,プレゼン
   ```

2. **ビルド・アップロード**

3. **初回起動時のWi-Fi設定**

   - スマートフォン等で SSID **`M5Tab5_Setup`** に接続
   - ブラウザが自動起動するキャプティブポータルからWi-Fi認証情報を入力
   - 設定は不揮発メモリに保存され、次回以降は自動接続
   - 接続後、NTPサーバと時刻同期し、RTCを更新

4. **Wi-Fiなしで使用する場合**

   シリアルモニタ（115200bps）から時刻を手動設定：

   ```txt
   2025/06/14 08:30:00
   ```

---

## 動作フロー

```txt
起動
 ↓
タイムゾーン設定（JST固定）
 ↓
RTC初期化
 ↓
Wi-Fi接続（WiFiManager、3分タイムアウト）
  ├─ 成功 → NTP同期 → RTC更新
  └─ 失敗/タイムアウト → RTC時刻で継続
 ↓
SDカード初期化 → スケジュール読み込み
 ↓
メインループ（1秒更新）
```

---

## 画面表示

| 領域 | 内容 |
| --- | ---- |
| 上部 | タイトル・現在時刻 |
| 右上 | RTC / SDカード / Wi-Fi / スケジュール件数 ステータス |
| 左下 | 進行中の予定（最大3件） |
| 右下 | 今後の予定（最大4件） |

---

## トラブルシューティング

| 症状 | 確認事項 |
| --- | ------- |
| RTC:NG 表示 | I2Cピン（SDA=31, SCL=32）・モジュール接続を確認 |
| SDカード:NG 表示 | SDカードの挿入・FAT32フォーマット・接触を確認 |
| スケジュールが表示されない | `/schedule.csv` の存在とCSV形式を確認 |
| Wi-Fi:NG 表示 | 再起動して `M5Tab5_Setup` APに接続し再設定 |
| NTP同期失敗 | Wi-Fi接続後もインターネット疎通を確認 |

---

## ライセンス

MIT License
