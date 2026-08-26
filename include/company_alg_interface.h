#ifndef COMPANY_ALG_INTERFACE_H_
#define COMPANY_ALG_INTERFACE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================
// 统一 C 风格字符串结构体 (包含数据指针、有效长度与缓冲区容量)
// =============================================================
typedef struct {
  char* data;       // 字符数据指针 (以 '\0' 结尾)
  size_t length;    // 字符串有效长度 (字节数，不含 '\0')
  size_t capacity;  // 缓冲区最大容量 (字节数，含 '\0')
} CompanyString;

/**
 * @brief 初始化 CompanyString 输出缓冲区
 */
static inline void CompanyString_Init(CompanyString* str, char* buffer,
                                      size_t capacity) {
  if (!str) return;
  str->data = buffer;
  str->capacity = capacity;
  str->length = 0;
  if (buffer && capacity > 0) {
    buffer[0] = '\0';
  }
}

/**
 * @brief 从只读 C 字符串初始化 CompanyString (用于只读输入包装)
 */
static inline void CompanyString_FromCString(CompanyString* str,
                                             const char* cstr) {
  if (!str) return;
  if (!cstr) {
    str->data = NULL;
    str->length = 0;
    str->capacity = 0;
    return;
  }
  size_t len = 0;
  while (cstr[len] != '\0') len++;
  str->data = (char*)cstr;
  str->length = len;
  str->capacity = len + 1;
}

// 业务类型枚举
typedef enum {
  ALG_BIZ_TYPE_UNKNOWN = 0,
  ALG_BIZ_TYPE_DOC_QA = 1,  // 业务 3: 智能长文档问答 (Embedding + LLM)
  ALG_BIZ_TYPE_KEYWORD_MATCH =
      2,  // 业务 1: 关注词匹配业务 (无模型, Control动态词表)
  ALG_BIZ_TYPE_ENTITY_EXTRACT = 3,  // 业务 2: 实体/名词提取 (0.6B LLM)
  ALG_BIZ_TYPE_COMPLIANCE_AUDIT =
      4,  // 业务 4: 智能对话风控质检 (Embedding + Reranker + LLM 多模型多节点)
  ALG_BIZ_TYPE_OCR_DOC_QA = 5,  // 业务 5: 智能多模态图文票据抽取 (OCR + LLM)
  ALG_BIZ_TYPE_AUDIO_ASR_INTENT =
      6,  // 业务 6: 语音识别与意图槽位抽取 (Audio ASR + NLU)
  ALG_BIZ_TYPE_CROSS_RERANK =
      7,  // 业务 7: 纯语义精排打分 (Query-Passage Cross-Encoder)
  // Force a 32-bit ABI representation and keep positive invalid-value probes
  // representable when enum sanitization is enabled.
  ALG_BIZ_TYPE_MAX_GUARD = INT32_MAX
} CompanyAlgBizType;

// 句柄创建参数结构体
typedef struct {
  const char* config_file_path;  // 业务配置文件路径 (JSON)
  const char* model_root_dir;    // 模型根目录
  int device_id;                 // 目标加速设备 ID (如 NPU 0, GPU 0)
  CompanyAlgBizType biz_type;    // 业务类型
} CompanyAlgParamCreate;

// 运行时动态控制参数结构体
typedef struct {
  int control_cmd;  // 1: 更新词表/规则, 2: 切换 Prompt, 3: 调整阈值
  const char* json_param_str;  // 控制参数的 JSON 字符串
} CompanyAlgParamControl;

// -------------------------------------------------------------
// 业务 1: 关注词匹配业务输入输出结构体
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;                 // 外部请求唯一 ID
  const CompanyString* sentence_text;  // 输入的一句话
} CompanyKeywordInputStruct;

typedef struct {
  uint64_t request_id;               // 对应的外部请求 ID
  int is_hit;                        // 1: 命中, 0: 未命中
  CompanyString* match_result_json;  // 命中分类与词结果 (JSON字符串)
  int status_code;                   // 0: 成功, 其他: 失败
} CompanyKeywordOutputStruct;

// -------------------------------------------------------------
// 业务 2: 实体/名词提取业务输入输出结构体 (0.6B LLM)
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;                 // 外部请求唯一 ID
  const CompanyString* sentence_text;  // 输入的一句话
} CompanyEntityInputStruct;

typedef struct {
  uint64_t request_id;           // 对应的外部请求 ID
  CompanyString* entities_json;  // 提取出的实体/名词列表 (JSON字符串)
  int status_code;               // 0: 成功, 其他: 失败
} CompanyEntityOutputStruct;

// -------------------------------------------------------------
// 业务 3: 智能长文档问答业务输入输出结构体 (多模型协同)
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;              // 外部请求唯一 ID
  const CompanyString* doc_text;    // 输入的长文本 / 文档
  const CompanyString* query_text;  // 用户的问题 / Query
} CompanyDocInputStruct;

typedef struct {
  uint64_t request_id;         // 对应的外部请求 ID
  CompanyString* intent_name;  // 识别出的意图类别
  float confidence;            // 置信度
  CompanyString* answer_text;  // 算法回答 / 摘要结果
  int chunk_count;             // 文档被拆分的切片数
  int status_code;             // 0: 成功, 其他: 失败错误码
} CompanyDocOutputStruct;

// -------------------------------------------------------------
// 业务 4: 智能对话风控质检业务输入输出结构体 (多模型: Embedding + Rerank + LLM)
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;             // 外部请求唯一 ID
  const CompanyString* user_text;  // 待质检的对话文本
  const CompanyString* channel_name;  // 业务渠道 (如 "VIP专线", "在线客服")
} CompanyAuditInputStruct;

typedef struct {
  uint64_t request_id;        // 对应的外部请求 ID
  CompanyString* risk_level;  // 风险等级: "HIGH_RISK", "MEDIUM_RISK", "SAFE"
  float risk_score;           // 综合风险打分 0.0 ~ 1.0
  CompanyString* matched_policy_clause;  // 语义检索+精排命中的合规制度条款
  CompanyString* audit_verdict_json;  // 审核结论与合规建议 (JSON格式)
  int status_code;                    // 0: 成功, 其他: 失败
} CompanyAuditOutputStruct;

// -------------------------------------------------------------
// 业务 5: 智能多模态图文票据问答结构体 (OCR Detection/Recog + LLM)
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;              // 外部请求唯一 ID
  const CompanyString* image_path;  // 待识别的图片路径或 URI
  const CompanyString*
      query_prompt;  // 结构化提取提问 (如 "请提取发票代码、金额与开票日期")
} CompanyOcrDocInputStruct;

typedef struct {
  uint64_t request_id;                    // 对应的外部请求 ID
  int detected_box_count;                 // OCR 检出的文字区块数量
  CompanyString* extracted_invoice_json;  // 最终 LLM 结构化提取的 JSON 结果
  int status_code;                        // 0: 成功, 其他: 失败
} CompanyOcrDocOutputStruct;

// -------------------------------------------------------------
// 业务 6: 智能语音交互与意图槽位抽取结构体 (Audio PCM Stream + ASR + NLU)
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;      // 外部请求唯一 ID
  const float* pcm_buffer;  // 原始 PCM 浮点音频流数据
  int pcm_length;           // 采样点长度
  int sample_rate;          // 采样率 (如 16000)
} CompanyAudioInputStruct;

typedef struct {
  uint64_t request_id;              // 对应的外部请求 ID
  CompanyString* transcribed_text;  // ASR 识别出的自然语言文本
  CompanyString* intent_slot_json;  // NLU 解析出的意图与槽位 JSON
  int status_code;                  // 0: 成功, 其他: 失败
} CompanyAudioOutputStruct;

// -------------------------------------------------------------
// 业务 7: 纯语义精排矩阵打分业务结构体 (1 Query + N Passages 批量Cross-Encoder)
// -------------------------------------------------------------
typedef struct {
  uint64_t request_id;                         // 外部请求唯一 ID
  const CompanyString* query_text;             // 用户提问
  const CompanyString* candidate_passages[8];  // 候选段落指针数组 (最多 8 个)
  int candidate_count;                         // 候选段落数量
} CompanyRerankBatchInputStruct;

typedef struct {
  uint64_t request_id;    // 对应的外部请求 ID
  float scores[8];        // 精排打分 (0.0 ~ 1.0)
  int sorted_indices[8];  // 按得分降序排列的原始索引
  int count;              // 结果数量
  int status_code;        // 0: 成功, 其他: 失败
} CompanyRerankBatchOutputStruct;

// -------------------------------------------------------------
// 槽位 "xxx.mapname" 中 mapname 到 C 解析结构体的元数据映射 (公司调度框架契约)
// -------------------------------------------------------------
typedef struct {
  const char* slot_mapname;  // 槽位 mapname 后缀 (如 "keyword_in", "doc_out")
  CompanyAlgBizType biz_type;  // 所属业务类型
  const char*
      struct_name;  // 对应的 C 结构体名称 (如 "CompanyKeywordInputStruct")
  size_t struct_size;  // 结构体大小 sizeof(...)
  int is_input;        // 1: 输入结构体, 0: 输出结构体
} CompanySlotStructMapping;

// -------------------------------------------------------------
// 公司统一标准 C ABI 错误码常量 (ABI V2, ADP-004)
// -------------------------------------------------------------
#define COMPANY_ALG_SUCCESS (0)              // 成功
#define COMPANY_ALG_ERR_INVALID_HANDLE (-1)  // 无效句柄 (nullptr 或野指针)
#define COMPANY_ALG_ERR_INVALID_PARAM \
  (-2)  // 参数非法 (如空字符串、缺少必填参数)
#define COMPANY_ALG_ERR_INVALID_INPUT \
  (-3)  // 输入数据非法 (空指针槽位、批大小超限或字段语义错误)
#define COMPANY_ALG_ERR_BUFFER_TOO_SMALL \
  (-4)  // 输出缓冲区不足或空槽位 (此时回填所需容量)
#define COMPANY_ALG_ERR_UNSUPPORTED_BIZ \
  (-5)  // 不支持或未注册的业务类型 / 业务配置不匹配
#define COMPANY_ALG_ERR_REGISTRY_CONFLICT (-6)  // 注册表冲突 (fail-closed 拦截)
#define COMPANY_ALG_ERR_EXCEPTION (-99)  // 运行时捕获到 std::exception 异常
#define COMPANY_ALG_ERR_UNKNOWN (-100)  // 运行时捕获到未知异常

#ifdef __cplusplus
#define COMPANY_ALG_NOEXCEPT noexcept
#else
#define COMPANY_ALG_NOEXCEPT
#endif

// -------------------------------------------------------------
// 公司统一限定导出的标准纯 C 接口 (ABI V2)
// -------------------------------------------------------------

/**
 * @brief 获取公司标准槽位 mapname 与 C 解析结构体的全局映射表
 * @param[out] count 返回映射项数量
 * @return 映射表静态数组首地址
 */
const CompanySlotStructMapping* Alg_GetSlotStructMappings(int* count)
    COMPANY_ALG_NOEXCEPT;

/**
 * @brief 全局资源初始化 (进程级)
 */
int Alg_Init(void) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 创建算法处理句柄实例 (会话级)
 * @param[out] hndl 返回的句柄指针
 * @param[in] param_create 创建参数
 */
int Alg_Create(void** hndl,
               const CompanyAlgParamCreate* param_create) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 批量执行算法计算 (纯 C 标准批处理接口)
 * @param[in] hndl 算法句柄
 * @param[in] inputs 多个输入结构体指针数组
 * @param[in] num_inputs 输入样本数量
 * @param[out] outputs 多个输出结构体指针数组
 * @param[in,out] num_outputs 输入为 outputs 容量，输出为实际填充的样本数量
 */
int Alg_Process(void* hndl, const void** inputs, int num_inputs, void** outputs,
                int* num_outputs) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 运行时动态控制或参数调整
 */
int Alg_Control(void* hndl, const CompanyAlgParamControl* param_control)
    COMPANY_ALG_NOEXCEPT;

/**
 * @brief 销毁算法句柄实例
 */
int Alg_Destroy(void* hndl) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 全局资源释放 (进程级)
 */
int Alg_DeInit(void) COMPANY_ALG_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif  // COMPANY_ALG_INTERFACE_H_
