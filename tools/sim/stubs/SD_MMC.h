// SD カードの代わりに sim::sd_root 配下のファイルを読む
#pragma once
#include "Arduino.h"

#define CARD_NONE 0
#define CARD_MMC  1
#define CARD_SD   2
#define CARD_SDHC 3

class File {
  FILE* fp_ = nullptr;
  int next_ = EOF;
public:
  File() = default;
  explicit File(FILE* fp) : fp_(fp) { if (fp_) next_ = fgetc(fp_); }
  explicit operator bool() const { return fp_ != nullptr; }
  int available() { return next_ != EOF; }
  int read() { int c = next_; next_ = fgetc(fp_); return c; }
  void close() { if (fp_) fclose(fp_); fp_ = nullptr; }
};

struct SimSD {
  bool begin(const char*, bool) { return true; }
  uint8_t cardType() { return CARD_SDHC; }
  uint64_t cardSize() { return 16ULL * 1024 * 1024 * 1024; }
  File open(const char* path) { return File(fopen((sim::sd_root + path).c_str(), "rb")); }
};
inline SimSD SD_MMC;
