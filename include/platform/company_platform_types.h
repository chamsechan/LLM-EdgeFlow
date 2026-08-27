#ifndef COMPANY_PLATFORM_TYPES_H_
#define COMPANY_PLATFORM_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 平台 C 字符串镜像结构 (显式长度与可变指针，零拷贝借用/池化持有)
 */
typedef struct CompanyString {
  int32_t length;  // 有效字节长度 (不含结尾可选 NUL)
  char* data;      // 指向字符数据区
} CompanyString;

/**
 * @brief 平台二进制通用缓冲区镜像结构
 */
typedef struct CompanyBuffer {
  int32_t length;  // 有效字节数
  uint8_t* data;   // 指向字节数据区
} CompanyBuffer;

/**
 * @brief 平台白名单类型泛型载荷镜像结构
 */
typedef struct CompanyAny {
  int32_t type_id;        // 载荷类型 ID (受配置白名单约束)
  int32_t element_count;  // 元素个数
  int32_t byte_length;  // 总字节长度 (必须等于 element_count * sizeof(element))
  void* data;           // 数据指针
} CompanyAny;

/**
 * @brief 图像帧镜像输入结构体 (Demo URI-backed 图像帧)
 */
typedef struct CompanyFrame {
  uint64_t request_id;
  const CompanyString* image_uri;
  const CompanyAny* metadata;
} CompanyFrame;

/**
 * @brief OCR / 目标检测结构化输出镜像结构体
 */
typedef struct CompanyOdOutput {
  uint64_t request_id;
  int32_t detected_box_count;
  CompanyString* result_json;
  CompanyAny* metadata;
  int32_t status_code;
} CompanyOdOutput;

/**
 * @brief 业务 4: 智能对话风控质检平台聚合输入结构体
 */
typedef struct CompanyPlatformAuditInput {
  uint64_t request_id;
  const CompanyString* user_text;
  const CompanyString* channel_name;
} CompanyPlatformAuditInput;

/**
 * @brief 业务 4: 智能对话风控质检平台聚合输出结构体
 */
typedef struct CompanyPlatformAuditOutput {
  uint64_t request_id;
  CompanyString* risk_level;
  float risk_score;
  CompanyString* matched_policy_clause;
  CompanyString* audit_verdict_json;
  int32_t status_code;
} CompanyPlatformAuditOutput;

/**
 * @brief 业务 1: 关注词匹配平台聚合输入结构体
 */
typedef struct CompanyPlatformKeywordInput {
  uint64_t request_id;
  const CompanyString* sentence_text;
} CompanyPlatformKeywordInput;

/**
 * @brief 业务 1: 关注词匹配平台聚合输出结构体
 */
typedef struct CompanyPlatformKeywordOutput {
  uint64_t request_id;
  int32_t is_hit;
  CompanyString* match_result_json;
  int32_t status_code;
} CompanyPlatformKeywordOutput;

/**
 * @brief 业务 2: 实体/名词提取平台聚合输入结构体
 */
typedef struct CompanyPlatformEntityInput {
  uint64_t request_id;
  const CompanyString* sentence_text;
} CompanyPlatformEntityInput;

/**
 * @brief 业务 2: 实体/名词提取平台聚合输出结构体
 */
typedef struct CompanyPlatformEntityOutput {
  uint64_t request_id;
  CompanyString* entities_json;
  int32_t status_code;
} CompanyPlatformEntityOutput;

/**
 * @brief 业务 3: 智能长文档问答平台聚合输入结构体
 */
typedef struct CompanyPlatformDocInput {
  uint64_t request_id;
  const CompanyString* doc_text;
  const CompanyString* query_text;
} CompanyPlatformDocInput;

/**
 * @brief 业务 3: 智能长文档问答平台聚合输出结构体
 */
typedef struct CompanyPlatformDocOutput {
  uint64_t request_id;
  CompanyString* intent_name;
  float confidence;
  CompanyString* answer_text;
  int32_t chunk_count;
  int32_t status_code;
} CompanyPlatformDocOutput;

/**
 * @brief 业务 6: 语音识别与意图抽取平台聚合输入结构体
 */
typedef struct CompanyPlatformAudioInput {
  uint64_t request_id;
  const float* pcm_buffer;
  int32_t pcm_length;
  int32_t sample_rate;
} CompanyPlatformAudioInput;

/**
 * @brief 业务 6: 语音识别与意图抽取平台聚合输出结构体
 */
typedef struct CompanyPlatformAudioOutput {
  uint64_t request_id;
  CompanyString* transcribed_text;
  CompanyString* intent_slot_json;
  int32_t status_code;
} CompanyPlatformAudioOutput;

#define COMPANY_PLATFORM_MAX_RERANK_CANDIDATES 8

/**
 * @brief 业务 7: 纯语义精排平台聚合输入结构体
 */
typedef struct CompanyPlatformRerankInput {
  uint64_t request_id;
  const CompanyString* query_text;
  const CompanyString*
      candidate_passages[COMPANY_PLATFORM_MAX_RERANK_CANDIDATES];
  int32_t candidate_count;
} CompanyPlatformRerankInput;

/**
 * @brief 业务 7: 纯语义精排平台聚合输出结构体
 */
typedef struct CompanyPlatformRerankOutput {
  uint64_t request_id;
  float scores[COMPANY_PLATFORM_MAX_RERANK_CANDIDATES];
  int32_t sorted_indices[COMPANY_PLATFORM_MAX_RERANK_CANDIDATES];
  int32_t count;
  int32_t status_code;
} CompanyPlatformRerankOutput;

#ifdef __cplusplus
}
#endif

#endif  // COMPANY_PLATFORM_TYPES_H_
