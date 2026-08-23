#include "demo/common/dataset_reader.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace alg_demo {

std::string ResolvePath(const std::string& rel_path) {
  if (rel_path.empty()) return "";
  FILE* fp = std::fopen(rel_path.c_str(), "r");
  if (fp) {
    std::fclose(fp);
    return rel_path;
  }
  std::string parent_path = "../" + rel_path;
  fp = std::fopen(parent_path.c_str(), "r");
  if (fp) {
    std::fclose(fp);
    return parent_path;
  }
  return rel_path;
}

std::string Trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

bool ReadLinesFromFile(const std::string& file_path,
                       std::vector<std::string>* out_lines,
                       std::string* error_msg) {
  if (!out_lines) {
    if (error_msg) *error_msg = "Null output vector pointer";
    return false;
  }
  out_lines->clear();

  std::string resolved = ResolvePath(file_path);
  std::ifstream ifs(resolved);
  if (!ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Cannot open dataset file: '" + file_path +
                   "' (resolved: '" + resolved + "')";
    }
    return false;
  }

  std::string line;
  while (std::getline(ifs, line)) {
    std::string trimmed = Trim(line);
    if (!trimmed.empty() && trimmed[0] != '#') {
      out_lines->push_back(trimmed);
    }
  }
  return true;
}

bool ParseTagSections(
    const std::string& file_path,
    std::unordered_map<std::string, std::vector<std::string>>* out_sections,
    std::string* error_msg) {
  if (!out_sections) {
    if (error_msg) *error_msg = "Null output sections map pointer";
    return false;
  }
  out_sections->clear();

  std::string resolved = ResolvePath(file_path);
  std::ifstream ifs(resolved);
  if (!ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Cannot open tag sections file: '" + file_path +
                   "' (resolved: '" + resolved + "')";
    }
    return false;
  }

  std::string current_tag = "DEFAULT";
  std::string line;
  while (std::getline(ifs, line)) {
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;

    if (trimmed.front() == '[' && trimmed.back() == ']' &&
        trimmed.size() >= 2) {
      current_tag = trimmed.substr(1, trimmed.size() - 2);
    } else {
      (*out_sections)[current_tag].push_back(trimmed);
    }
  }
  return true;
}

bool ReadTextFile(const std::string& file_path, std::string* out_content,
                  std::string* error_msg) {
  if (!out_content) {
    if (error_msg) *error_msg = "Null output content pointer";
    return false;
  }
  out_content->clear();

  std::string resolved = ResolvePath(file_path);
  std::ifstream ifs(resolved);
  if (!ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Cannot open text file: '" + file_path + "' (resolved: '" +
                   resolved + "')";
    }
    return false;
  }

  std::stringstream ss;
  ss << ifs.rdbuf();
  *out_content = ss.str();
  return true;
}

void PrintBanner(const std::string& title, const std::string& subtitle) {
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

void PrintDivider() {
  std::cout
      << "------------------------------------------------------------------"
      << std::endl;
}

}  // namespace alg_demo
