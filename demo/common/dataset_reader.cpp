#include "demo/common/dataset_reader.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
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

bool ReadAudioDataset(const std::string& manifest_path,
                      std::vector<AudioDatasetSample>* out_samples,
                      std::string* error_msg) {
  if (!out_samples) {
    if (error_msg) *error_msg = "Null output samples pointer";
    return false;
  }
  out_samples->clear();

  std::string resolved = ResolvePath(manifest_path);
  std::ifstream ifs(resolved);
  if (!ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Cannot open manifest file: '" + manifest_path +
                   "' (resolved: '" + resolved + "')";
    }
    return false;
  }

  std::filesystem::path manifest_dir =
      std::filesystem::path(resolved).parent_path();

  std::string line;
  size_t line_num = 0;
  while (std::getline(ifs, line)) {
    ++line_num;
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;

    nlohmann::json item = nlohmann::json::parse(trimmed, nullptr, false);
    if (item.is_discarded() || !item.is_object()) {
      if (error_msg) {
        *error_msg = "Invalid JSON on line " + std::to_string(line_num);
      }
      out_samples->clear();
      return false;
    }

    if (!item.contains("request_id") ||
        !item["request_id"].is_number_unsigned() ||
        !item.contains("pcm_f32le") || !item["pcm_f32le"].is_string() ||
        !item.contains("sample_rate") ||
        !item["sample_rate"].is_number_integer()) {
      if (error_msg) {
        *error_msg = "Missing or invalid required fields on line " +
                     std::to_string(line_num);
      }
      out_samples->clear();
      return false;
    }

    AudioDatasetSample sample;
    sample.request_id = item["request_id"].get<uint64_t>();
    sample.pcm_f32le = item["pcm_f32le"].get<std::string>();
    sample.sample_rate = item["sample_rate"].get<int>();
    if (item.contains("reference_text") && item["reference_text"].is_string()) {
      sample.reference_text = item["reference_text"].get<std::string>();
    }
    if (item.contains("expected_category") &&
        item["expected_category"].is_string()) {
      sample.expected_category = item["expected_category"].get<std::string>();
    }

    if (sample.sample_rate != 16000) {
      if (error_msg) {
        *error_msg = "Sample rate on line " + std::to_string(line_num) +
                     " must be 16000";
      }
      out_samples->clear();
      return false;
    }

    std::filesystem::path audio_path = manifest_dir / sample.pcm_f32le;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(audio_path, ec)) {
      if (error_msg) {
        *error_msg =
            "Audio file not found or not regular file: " + audio_path.string();
      }
      out_samples->clear();
      return false;
    }

    auto file_size = std::filesystem::file_size(audio_path, ec);
    if (ec || file_size == 0 || file_size % sizeof(float) != 0) {
      if (error_msg) {
        *error_msg =
            "Audio file size is invalid (not multiple of 4 or empty): " +
            audio_path.string();
      }
      out_samples->clear();
      return false;
    }

    const size_t n_samples = file_size / sizeof(float);
    if (n_samples < 1600 || n_samples > 960000) {
      if (error_msg) {
        *error_msg = "Audio duration out of bounds (100ms to 60s): " +
                     audio_path.string();
      }
      out_samples->clear();
      return false;
    }

    std::ifstream audio_file(audio_path, std::ios::binary);
    if (!audio_file.is_open()) {
      if (error_msg) {
        *error_msg = "Failed to open audio file: " + audio_path.string();
      }
      out_samples->clear();
      return false;
    }

    sample.pcm_data.resize(n_samples);
    audio_file.read(reinterpret_cast<char*>(sample.pcm_data.data()),
                    static_cast<std::streamsize>(file_size));
    if (!audio_file) {
      if (error_msg) {
        *error_msg =
            "Failed to read complete audio file: " + audio_path.string();
      }
      out_samples->clear();
      return false;
    }

    for (float s : sample.pcm_data) {
      if (!std::isfinite(s) || s < -1.0f || s > 1.0f) {
        if (error_msg) {
          *error_msg =
              "Invalid PCM sample amplitude in: " + audio_path.string();
        }
        out_samples->clear();
        return false;
      }
    }

    out_samples->push_back(std::move(sample));
  }

  if (out_samples->empty()) {
    if (error_msg) {
      *error_msg = "Manifest file contained no valid samples: " + manifest_path;
    }
    return false;
  }
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
