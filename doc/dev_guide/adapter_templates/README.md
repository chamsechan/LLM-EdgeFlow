# 📚 Adapter 4 大复杂数据结构标准开发范式与可编译模板 (ADP-010, RECHECK-005)

针对企业级 C ABI 中常见的 4 类复杂数据结构，`LLM-EdgeFlow` 提供了统一的安全解析规范与标准开发模板。所有模板均包含独立的纯 C 结构体定义、内部 DTO、`IBizAdapter` 完整实现与契约测试。

所有模板代码均在工程中作为真实头文件维护并直接参与自动化编译与测试：
- 范式一 (Flat Struct)：[`include/adapter/templates/flat_struct_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/flat_struct_adapter.h)
- 范式二 (Tagged Union)：[`include/adapter/templates/tagged_union_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/tagged_union_adapter.h)
- 范式三 (Nested Dynamic Array)：[`include/adapter/templates/nested_array_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/nested_array_adapter.h)
- 范式四 (Nested Pointer Tree)：[`include/adapter/templates/nested_pointer_tree_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/nested_pointer_tree_adapter.h)

---

## 1. 范式一：平面定长结构体 (Flat Struct)

**适用场景**：包含基础标量字段与固定长度字符数组/Buffer。

```cpp
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_interface.h"

namespace alg_framework {
namespace template_examples {

// 1. 纯 C 结构体声明
typedef struct {
  uint64_t request_id;
  const char* sentence_text;  // 必填 UTF-8 字符串
} TemplateFlatInput;

typedef struct {
  uint64_t request_id;
  int status_code;
  char result_json[512];  // 固定容量输出
} TemplateFlatOutput;

// 2. 内部 DTO
struct TemplateFlatResultDto {
  uint64_t request_id;
  int status_code;
  std::string result_json;
};

// 3. 适配器标准实现
class TemplateFlatStructAdapter : public IBizAdapter {
 public:
  CompanyAlgBizType BizType() const override { return static_cast<CompanyAlgBizType>(101); }
  const char* BizName() const override { return "TemplateFlatStruct"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        static_cast<CompanyAlgBizType>(101), "TemplateFlatStruct", "2.0.0",
        "TemplateFlatInput", "TemplateFlatOutput", 64,
        OwnershipPolicy::kCopyIn, ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne, {"template_flat_pipeline_v1"}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> req_ids;
    std::vector<std::string> sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const TemplateFlatInput*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireBoundedString("inputs[i].sentence_text", in->sentence_text, 64 * 1024, i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      req_ids.push_back(in->request_id);
      sentences.push_back(in->sentence_text); // COPY_IN 深拷贝
    }
    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, "raw_request_ids", std::move(req_ids), BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, "raw_sentences", std::move(sentences), BizName(), out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    auto* res = ctx->Read<std::vector<TemplateFlatResultDto>>("flat_final_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<TemplateFlatOutput*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 校验截断并严格拦截
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->result_json, sizeof(out_ptr->result_json),
              (*res)[i].result_json.c_str(), "outputs[i].result_json", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

}  // namespace template_examples
}  // namespace alg_framework
```

---

## 2. 范式二：带标签联合体 (Tagged Union / 多态载荷)

**适用场景**：根据 `payload_type` 枚举动态决定 payload 联合体或指针结构。

参考实现：[`include/adapter/templates/tagged_union_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/tagged_union_adapter.h)。

关键安全要求：
1. 必须使用 `AdapterValidationHelper::RequireEnum` 对标签枚举进行强校验，杜绝非法枚举值导致内存未定义访问。
2. 每一个分支必须独立校验其内部指针与数值范围（如图片宽高、文本长度），并在 `Unpack` 中深拷贝至 `AlgContext`。

---

## 3. 范式三：嵌套变长数组与乘法溢出防护 (Nested Dynamic Array)

**适用场景**：包含指针指向的变长结构体数组或矩阵（如检测框集合、标签列表）。

参考实现：[`include/adapter/templates/nested_array_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/nested_array_adapter.h)。

关键安全要求：
1. 必须检查 `tag_count >= 0` 以及上限阈值，杜绝负数长度或极端大整数。
2. 必须使用 `AdapterValidationHelper::CheckedMultiply` 校验 `count * sizeof(T)` 是否溢出，并设定总体最大字节限制。
3. 数组元素中的字符串必须逐个通过 `RequireBoundedString` 扫描。

---

## 4. 范式四：多级嵌套指针树 (Nested Pointer Tree)

**适用场景**：树状文档或多级递归结构（如 Document -> Sections -> Paragraphs）。

参考实现：[`include/adapter/templates/nested_pointer_tree_adapter.h`](file:///home/ubuntu/project/llm-ops-agy/include/adapter/templates/nested_pointer_tree_adapter.h)。

关键安全要求：
1. 必须显式限制最大递归深度（如 `max_depth = 32`），防止恶意循环引用或深层嵌套导致调用栈溢出（Stack Overflow）。
2. 每层节点必须独立校验非空、子节点数量范围和多级指针有效性。
3. 树结构数据在 `Unpack` 递归过程中必须完整深拷贝为 C++ STL 树状 DTO。
