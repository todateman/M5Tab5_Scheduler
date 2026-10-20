// CSV_Parser の代わり（文字列列 "s" とヘッダー行のみ対応）
#pragma once
#include <cstring>
#include <string>
#include <vector>

class CSV_Parser {
  std::vector<std::vector<std::string>> cells_;   // [列][行]
  std::vector<std::vector<char*>> ptrs_;
public:
  CSV_Parser(char* data, const char* format, bool hasHeader, char delimiter) {
    size_t cols = strlen(format);
    cells_.resize(cols);
    std::string text(data);
    size_t pos = 0;
    bool header = hasHeader;
    while (pos < text.size()) {
      size_t eol = text.find('\n', pos);
      if (eol == std::string::npos) eol = text.size();
      std::string line = text.substr(pos, eol - pos);
      pos = eol + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (header) { header = false; continue; }
      size_t start = 0;
      for (size_t c = 0; c < cols; c++) {
        size_t comma = line.find(delimiter, start);
        cells_[c].push_back(line.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        start = (comma == std::string::npos) ? line.size() : comma + 1;
      }
    }
    ptrs_.resize(cols);
    for (size_t c = 0; c < cols; c++) {
      for (auto& s : cells_[c]) ptrs_[c].push_back(&s[0]);
    }
  }
  void* operator[](int col) { return ptrs_[(size_t)col].data(); }
  int getRowsCount() const { return cells_.empty() ? 0 : (int)cells_[0].size(); }
};
