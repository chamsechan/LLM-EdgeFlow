#pragma once

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace alg_demo {

// 路径解析工具：支持当前目录与上级目录相对路径自适应查找
inline std::string ResolvePath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  std::string parent_path = "../" + rel_path;
  fp = fopen(parent_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return parent_path;
  }
  return rel_path;
}

// 字符串去除首尾空白
inline std::string Trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// 从文本文件读取所有非空行
inline std::vector<std::string> ReadLinesFromFile(
    const std::string& file_path) {
  std::vector<std::string> lines;
  std::ifstream ifs(ResolvePath(file_path));
  if (!ifs.is_open()) {
    return lines;
  }
  std::string line;
  while (std::getline(ifs, line)) {
    std::string trimmed = Trim(line);
    if (!trimmed.empty() && trimmed[0] != '#') {
      lines.push_back(trimmed);
    }
  }
  return lines;
}

// 解析带有 [TAG] 标记段落的结构化测试语料
inline std::unordered_map<std::string, std::vector<std::string>>
ParseTagSections(const std::string& file_path) {
  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::ifstream ifs(ResolvePath(file_path));
  if (!ifs.is_open()) {
    return sections;
  }

  std::string current_tag = "DEFAULT";
  std::string line;
  while (std::getline(ifs, line)) {
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
      current_tag = trimmed.substr(1, trimmed.size() - 2);
    } else {
      sections[current_tag].push_back(trimmed);
    }
  }
  return sections;
}

// 终端高亮与分界排版辅助
inline void PrintBanner(const std::string& title,
                        const std::string& subtitle = "") {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout << "  " << title << std::endl;
  if (!subtitle.empty()) {
    std::cout << "  " << subtitle << std::endl;
  }
  std::cout
      << "=================================================================="
      << std::endl;
}

inline void PrintDivider() {
  std::cout
      << "------------------------------------------------------------------"
      << std::endl;
}

}  // namespace alg_demo
