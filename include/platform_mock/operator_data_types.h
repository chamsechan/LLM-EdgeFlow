#ifndef EDGEFLOW_PLATFORM_MOCK_OPERATOR_DATA_TYPES_H_
#define EDGEFLOW_PLATFORM_MOCK_OPERATOR_DATA_TYPES_H_

// 本仓库 Demo 和测试使用的本地平台 mock 声明。
// 它们是现有外部环境的替代品，不是公司 SDK 头文件。
// Process 的输入输出按 vector 行位置对应，载体不含请求 ID 字段。

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 外网测试占位值，进入内网后必须核对真实 SDK 枚举。
enum {
  kMockServiceKeywordMatch = 1,
  kMockServiceEntityExtract = 2,
  kMockServiceDocQa = 3,
  kMockServiceDialogueAudit = 4,
  kMockServiceOcrInvoiceQa = 5,
  kMockServiceAudioAsrIntent = 6,
  kMockServiceCrossRerank = 7,
  kMockServiceTranslate = 8
};

/**
 * @brief Operator C 字符串镜像结构 (显式长度与可变指针，零拷贝借用/池化持有)
 */
typedef struct CompanyString {
  int32_t length;  // 有效字节长度 (不含结尾可选 NUL)
  char* data;      // 指向字符数据区
} CompanyString;

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
 * @brief RGB8 图像帧镜像输入结构体 (行交错，借用至 Process 返回)
 */
typedef struct CompanyFrame {
  int32_t service_type;
  int32_t height;
  int32_t width;
  int32_t stride;  // 每行字节数，至少 width * 3
  const void* data;  // 调用方保证 height * stride 字节可读，调用期间不修改
  const CompanyAny* metadata;
} CompanyFrame;

/**
 * @brief 统一业务输出；JSON 字符串由输出池持有，有效期随输出租约。
 */
typedef struct CompanyOperatorOutput {
  int32_t service_type;
  CompanyString* result_json;
  int32_t status_code;
} CompanyOperatorOutput;

/**
 * @brief 业务 4: 对话合规审核 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorAuditInput {
  int32_t service_type;
  const CompanyString* user_text;
  const CompanyString* channel_name;
} CompanyOperatorAuditInput;

/**
 * @brief 业务 1: 关注词匹配 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorKeywordInput {
  int32_t service_type;
  const CompanyString* sentence_text;
} CompanyOperatorKeywordInput;

/**
 * @brief 业务 2: 实体抽取 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorEntityInput {
  int32_t service_type;
  const CompanyString* sentence_text;
} CompanyOperatorEntityInput;

/**
 * @brief 业务 3: 文档问答 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorDocInput {
  int32_t service_type;
  const CompanyString* doc_text;
  const CompanyString* query_text;
} CompanyOperatorDocInput;

/**
 * @brief 业务 6: 语音识别与意图抽取 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorAudioInput {
  int32_t service_type;
  const float* pcm_buffer;
  int32_t pcm_length;
  int32_t sample_rate;
} CompanyOperatorAudioInput;

#define COMPANY_OPERATOR_MAX_RERANK_CANDIDATES 8

/**
 * @brief 业务 7: Cross-Encoder 精排 Operator 聚合输入结构体
 */
typedef struct CompanyOperatorRerankInput {
  int32_t service_type;
  const CompanyString* query_text;
  const CompanyString*
      candidate_passages[COMPANY_OPERATOR_MAX_RERANK_CANDIDATES];
  int32_t candidate_count;
} CompanyOperatorRerankInput;

#ifdef __cplusplus
}
#endif

#endif  // EDGEFLOW_PLATFORM_MOCK_OPERATOR_DATA_TYPES_H_
