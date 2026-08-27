#ifndef COMPANY_OPERATOR_TYPES_H_
#define COMPANY_OPERATOR_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operator C 字符串镜像结构 (显式长度与可变指针，零拷贝借用/池化持有)
 */
typedef struct CompanyString {
  int32_t length;  // 有效字节长度 (不含结尾可选 NUL)
  char* data;      // 指向字符数据区
} CompanyString;

/**
 * @brief Operator 二进制通用缓冲区镜像结构
 */
typedef struct CompanyBuffer {
  int32_t length;  // 有效字节数
  uint8_t* data;   // 指向字节数据区
} CompanyBuffer;

/**
 * @brief Operator 白名单类型泛型载荷镜像结构
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
 * @brief 业务 4: 智能对话风控质检 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorAuditInput {
  uint64_t request_id;
  const CompanyString* user_text;
  const CompanyString* channel_name;
} CompanyOperatorAuditInput;

/**
 * @brief 业务 4: 智能对话风控质检 Operator 聚合输出结构体
 */
typedef struct CompanyOperatorAuditOutput {
  uint64_t request_id;
  CompanyString* risk_level;
  float risk_score;
  CompanyString* matched_policy_clause;
  CompanyString* audit_verdict_json;
  int32_t status_code;
} CompanyOperatorAuditOutput;

/**
 * @brief 业务 1: 关注词匹配 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorKeywordInput {
  uint64_t request_id;
  const CompanyString* sentence_text;
} CompanyOperatorKeywordInput;

/**
 * @brief 业务 1: 关注词匹配 Operator 聚合输出结构体
 */
typedef struct CompanyOperatorKeywordOutput {
  uint64_t request_id;
  int32_t is_hit;
  CompanyString* match_result_json;
  int32_t status_code;
} CompanyOperatorKeywordOutput;

/**
 * @brief 业务 2: 实体/名词提取 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorEntityInput {
  uint64_t request_id;
  const CompanyString* sentence_text;
} CompanyOperatorEntityInput;

/**
 * @brief 业务 2: 实体/名词提取 Operator 聚合输出结构体
 */
typedef struct CompanyOperatorEntityOutput {
  uint64_t request_id;
  CompanyString* entities_json;
  int32_t status_code;
} CompanyOperatorEntityOutput;

/**
 * @brief 业务 3: 智能长文档问答 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorDocInput {
  uint64_t request_id;
  const CompanyString* doc_text;
  const CompanyString* query_text;
} CompanyOperatorDocInput;

/**
 * @brief 业务 3: 智能长文档问答 Operator 聚合输出结构体
 */
typedef struct CompanyOperatorDocOutput {
  uint64_t request_id;
  CompanyString* intent_name;
  float confidence;
  CompanyString* answer_text;
  int32_t chunk_count;
  int32_t status_code;
} CompanyOperatorDocOutput;

/**
 * @brief 业务 6: 语音识别与意图抽取 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorAudioInput {
  uint64_t request_id;
  const float* pcm_buffer;
  int32_t pcm_length;
  int32_t sample_rate;
} CompanyOperatorAudioInput;

/**
 * @brief 业务 6: 语音识别与意图抽取 Operator 聚合输出结构体
 */
typedef struct CompanyOperatorAudioOutput {
  uint64_t request_id;
  CompanyString* transcribed_text;
  CompanyString* intent_slot_json;
  int32_t status_code;
} CompanyOperatorAudioOutput;

#define COMPANY_OPERATOR_MAX_RERANK_CANDIDATES 8

/**
 * @brief 业务 7: 纯语义精排 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorRerankInput {
  uint64_t request_id;
  const CompanyString* query_text;
  const CompanyString*
      candidate_passages[COMPANY_OPERATOR_MAX_RERANK_CANDIDATES];
  int32_t candidate_count;
} CompanyOperatorRerankInput;

/**
 * @brief 业务 7: 纯语义精排 Operator 聚合输出结构体
 */
typedef struct CompanyOperatorRerankOutput {
  uint64_t request_id;
  float scores[COMPANY_OPERATOR_MAX_RERANK_CANDIDATES];
  int32_t sorted_indices[COMPANY_OPERATOR_MAX_RERANK_CANDIDATES];
  int32_t count;
  int32_t status_code;
} CompanyOperatorRerankOutput;

#ifdef __cplusplus
}
#endif

#endif  // COMPANY_OPERATOR_TYPES_H_
