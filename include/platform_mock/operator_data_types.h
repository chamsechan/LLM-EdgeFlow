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

// -------------------------------------------------------------
// 通用输入 envelope：MockInput -> MockDataPool -> MockTextInput
// 当前只承载文本。每层 byte_size 必须等于该层 sizeof；binding 依次校验
// root、data pool、文本长度，再复制到中性的 TextInputValue。全部借用至
// Process 返回。
// -------------------------------------------------------------

enum MockInputType { kMockInputMultidata = 1 };
enum MockDataType { kMockDataText = 1 };

typedef struct MockTextInput {
  int32_t byte_size;
  const char* value;  // length > 0 时必须非空；不要求 NUL 结尾，不得内嵌 NUL
  int32_t length;  // 有效字节长度
} MockTextInput;

typedef struct MockDataPool {
  int32_t byte_size;
  int32_t data_type;     // MockDataType
  const void* data_ptr;  // kMockDataText 时指向 MockTextInput
} MockDataPool;

typedef struct MockInput {
  int32_t byte_size;
  int32_t type;       // MockInputType，当前只接受 kMockInputMultidata
  int32_t num_frame;  // 当前只接受 1
  const MockDataPool* data_pool;
} MockInput;

// -------------------------------------------------------------
// 文本输出：MockOutput -> MockTextOutput -> 字符缓冲区
// 三者由输出池按块持有，有效期随输出租约。缓冲区容量来自外部参数
// result_json_max_bytes（另加 1 字节结尾 NUL），不在结构中公开。
// 复用前 length 与首字节清零；写入超容量即失败，不写入部分内容。
// -------------------------------------------------------------

enum MockOutputType { kMockOutputOc = 1 };

typedef struct MockTextOutput {
  int32_t byte_size;
  char* value;     // 池持有；成功写入后以 NUL 结尾
  int32_t length;  // 有效字节长度，不含结尾 NUL
} MockTextOutput;

typedef struct MockOutput {
  int32_t byte_size;
  int32_t type;  // MockOutputType
  int32_t service_type;
  MockTextOutput* text;
} MockOutput;

// -------------------------------------------------------------
// 以下非文本或多字段输入各自使用独立结构，不经过上面的通用 envelope。
// -------------------------------------------------------------

/**
 * @brief 显式长度字符串（借用至 Process 返回）
 */
typedef struct MockString {
  int32_t length;  // 有效字节长度（不含结尾可选 NUL）
  char* data;      // 指向字符数据区
} MockString;

/**
 * @brief 白名单类型泛型载荷
 */
typedef struct MockAny {
  int32_t type_id;        // 载荷类型 ID（受配置白名单约束）
  int32_t element_count;  // 元素个数
  int32_t byte_length;  // 总字节长度，须等于 element_count * sizeof(element)
  void* data;           // 数据指针
} MockAny;

/**
 * @brief RGB8 图像帧输入（行交错，借用至 Process 返回）
 */
typedef struct MockFrameInput {
  int32_t service_type;
  int32_t height;
  int32_t width;
  int32_t stride;  // 每行字节数，至少 width * 3
  const void* data;  // 调用方保证 height * stride 字节可读，调用期间不修改
  const MockAny* metadata;
} MockFrameInput;

/**
 * @brief 业务 4: 对话合规审核输入
 */
typedef struct MockAuditInput {
  int32_t service_type;
  const MockString* user_text;
  const MockString* channel_name;
} MockAuditInput;

/**
 * @brief 业务 3: 文档问答输入
 */
typedef struct MockDocInput {
  int32_t service_type;
  const MockString* doc_text;
  const MockString* query_text;
} MockDocInput;

/**
 * @brief 业务 6: 语音识别与意图抽取输入
 */
typedef struct MockAudioInput {
  int32_t service_type;
  const float* pcm_buffer;
  int32_t pcm_length;
  int32_t sample_rate;
} MockAudioInput;

#define MOCK_MAX_RERANK_CANDIDATES 8

/**
 * @brief 业务 7: Cross-Encoder 精排输入
 */
typedef struct MockRerankInput {
  int32_t service_type;
  const MockString* query_text;
  const MockString* candidate_passages[MOCK_MAX_RERANK_CANDIDATES];
  int32_t candidate_count;
} MockRerankInput;

#ifdef __cplusplus
}
#endif

#endif  // EDGEFLOW_PLATFORM_MOCK_OPERATOR_DATA_TYPES_H_
