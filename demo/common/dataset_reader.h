#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace alg_demo {

/**
 * @brief 自适应路径解析：在当前目录与上级目录查找相对路径文件
 * @param rel_path 相对路径
 * @return 匹配的绝对或相对路径
 */
std::string ResolvePath(const std::string& rel_path);

/**
 * @brief 去除字符串首尾空白字符
 */
std::string Trim(const std::string& s);

/**
 * @brief 从文本文件中按行读取非空且非注释（#）的样本
 * @param file_path 文件路径
 * @param out_lines 输出行列表
 * @param error_msg 错误输出信息
 * @return true 成功, false 失败 (文件无法打开或不存在)
 */
bool ReadLinesFromFile(const std::string& file_path,
                       std::vector<std::string>* out_lines,
                       std::string* error_msg = nullptr);

/**
 * @brief 解析带有 [TAG] 标记段落的结构化测试语料
 * @param file_path 文件路径
 * @param out_sections 输出标签段落字典
 * @param error_msg 错误输出信息
 * @return true 成功, false 失败
 */
bool ParseTagSections(
    const std::string& file_path,
    std::unordered_map<std::string, std::vector<std::string>>* out_sections,
    std::string* error_msg = nullptr);

/**
 * @brief 读取完整文本文件内容 (如 Control JSON 文件)
 * @param file_path 文件路径
 * @param out_content 输出文件字符串
 * @param error_msg 错误输出信息
 * @return true 成功, false 失败
 */
bool ReadTextFile(const std::string& file_path, std::string* out_content,
                  std::string* error_msg = nullptr);

struct AudioDatasetSample {
  uint64_t request_id = 0;
  std::string pcm_f32le;
  int sample_rate = 16000;
  std::string reference_text;
  std::string expected_category;
  std::vector<float> pcm_data;
};

/**
 * @brief 从 UTF-8 JSONL 清单与原始 little-endian float32 PCM 文件读取音频数据集
 */
bool ReadAudioDataset(const std::string& manifest_path,
                      std::vector<AudioDatasetSample>* out_samples,
                      std::string* error_msg = nullptr);

/**
 * @brief 终端高亮 Banner 与分割线排版
 */
void PrintBanner(const std::string& title, const std::string& subtitle = "");
void PrintDivider();

}  // namespace alg_demo
