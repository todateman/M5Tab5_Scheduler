# M5Tab5 スケジューラ表示システム

## 概要

このプロジェクトは、M5Stack Tab5（M5Unifiedライブラリ対応）上で、SDカードからCSV形式のスケジュールを読み込み、現在時刻・進行中・今後の予定を大画面に分かりやすく表示するシステムです。  
RTC（RX8130CE）を利用し、正確な時刻管理が可能です。

---

## 特徴

- SDカードからCSVファイルでスケジュールを簡単に反映
- Tab5内蔵のRTC（RX8130CE）でバッテリーを外しても時刻を保持
- 進行中・今後の予定をそれぞれ表示
- シリアル経由でRTC時刻設定が可能

---

## 必要なハードウェア

- M5Stack Tab5
- RX8130CE RTCモジュール（I2C接続）
- microSDカード（schedule.csv配置用）

---

## 必要なライブラリ

- [M5Unified](https://github.com/m5stack/M5Unified)
- [ArtronShop_RX8130CE](https://github.com/ArtronShop/ArtronShop_RX8130CE)
- [CSV_Parser](https://github.com/michalmonday/CSV-Parser-for-Arduino/)

---

## セットアップ手順

1. **ライブラリをインストール**
    - PlatformIOまたはArduino IDEで上記ライブラリを導入

2. **SDカードにスケジュールCSVを配置**
    - ファイル名：`/schedule.csv`
    - 例:
      ```
      start,stop,action
      2025/6/8 9:00,2025/6/8 10:00,会議
      2025/6/8 14:00,2025/6/8 15:00,プレゼン
      ```

3. **ビルド・アップロード・実行**


4. **シリアルでRTC時刻を設定（初回のみ）**
    - 例: `2025/06/14 08:30:00`
---

## 画面表示

- 現在時刻・RTC/SDカード/スケジュールの状態
- 進行中の予定（最大4件）
- 今後の予定（最大4件、残りは「...他 n 件」と表示）

---

## トラブルシューティング

- RTC初期化失敗時はピン番号を確認
- SDカード認識失敗時は挿入・フォーマット・配線を確認
- スケジュールが表示されない場合はCSVファイルの存在と形式を確認

---

## 今後のアップデート計画

- Wi-Fiに接続し、NTPサーバと時刻同期

---

## ライセンス

MIT License
