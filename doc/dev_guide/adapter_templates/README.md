# 📚 Adapter 4 大复杂数据结构标准开发范式与代码模板 (ADP-010)

针对企业级 C ABI 中常见的 4 类数据结构，`LLM-EdgeFlow` 提供统一的安全解析规范与标准开发模板：

---

## 1. 范式一：平面定长结构体 (Flat Struct)

适用场景：仅包含基础标量字段与固定长度字符数组/Buffer。

```cpp
// 纯 C 结构体声明
typedef struct {
  uint64_t request_id;
  const char* sentence_text;  // 必填 UTF-8 字符串
} CompanySentenceInput;

typedef struct {
  uint64_t request_id;
  int status_code;
  char result_json[2048];     // 固定容量输出
} CompanySentenceOutput;

// 适配器标准实现
class FlatStructAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_MY_BIZ; }
  const char* BizName() const override { return "MyFlatBiz"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_MY_BIZ, "MyFlatBiz", "2.0.0",
        "CompanySentenceInput", "CompanySentenceOutput", 64,
        OwnershipPolicy::kCopyIn, ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> req_ids;
    std::vector<std::string> sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanySentenceInput*>(inputs[i]);
      if (!in || !in->sentence_text) return COMPANY_ALG_ERR_INVALID_INPUT;

      req_ids.push_back(in->request_id);
      sentences.push_back(in->sentence_text); // COPY_IN 深拷贝
    }

    ctx->Set("request_ids", std::move(req_ids));
    ctx->Set("sentences", std::move(sentences));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    auto* res = ctx->Get<std::vector<MyResultDto>>("biz_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanySentenceOutput*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->result_json, sizeof(out_ptr->result_json),
          (*res)[i].result_json.c_str(), "outputs[i].result_json", i, BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};
```

---

## 2. 范式二：带标签联合体 (Tagged Union / 多态载荷)

适用场景：根据 `modal_type` 枚举动态决定 payload 联合体或指针结构。

```cpp
typedef enum {
  MODAL_TEXT = 1,
  MODAL_AUDIO = 2,
  MODAL_IMAGE = 3
} ModalType;

typedef struct {
  uint64_t request_id;
  int modal_type;
  union {
    const char* text_content;
    struct {
      const int16_t* pcm_data;
      int pcm_len;
      int sample_rate;
    } audio;
    struct {
      const uint8_t* img_bytes;
      size_t img_size;
    } image;
  } payload;
} CompanyMultiModalInput;

// 适配器标准实现
class TaggedUnionAdapter : public IBusinessAdapter {
 public:
  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) const override {
    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyMultiModalInput*>(inputs[i]);
      if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

      // 1. 严格校验枚举标签
      if (!AdapterValidationHelper::RequireEnum("inputs[i].modal_type", in->modal_type,
                                               {MODAL_TEXT, MODAL_AUDIO, MODAL_IMAGE})) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // 2. 根据枚举分支进行精准字段校验与 COPY_IN 深拷贝
      switch (in->modal_type) {
        case MODAL_TEXT: {
          if (!in->payload.text_content) return COMPANY_ALG_ERR_INVALID_INPUT;
          // 处理文本...
          break;
        }
        case MODAL_AUDIO: {
          if (in->payload.audio.pcm_len > 0 && !in->payload.audio.pcm_data) {
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          if (in->payload.audio.sample_rate <= 0) return COMPANY_ALG_ERR_INVALID_INPUT;
          // 处理音频...
          break;
        }
        case MODAL_IMAGE: {
          if (!in->payload.image.img_bytes || in->payload.image.img_size == 0) {
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          // 处理图像...
          break;
        }
      }
    }
    return COMPANY_ALG_SUCCESS;
  }
};
```

---

## 3. 范式三：嵌套变长数组与乘法溢出防护 (Nested Dynamic Array)

适用场景：包含指针指向的变长结构体数组或矩阵（如检测框集合、候选段落列表）。

```cpp
typedef struct {
  float x, y, w, h;
  float confidence;
} BoundingBox;

typedef struct {
  uint64_t request_id;
  int box_count;              // 变长数组大小
  const BoundingBox* boxes;   // 指向连续数组
} CompanyDetectionInput;

// 适配器标准实现
class NestedArrayAdapter : public IBusinessAdapter {
 public:
  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) const override {
    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyDetectionInput*>(inputs[i]);
      if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

      // 1. 校验数量与空指针组合
      if (in->box_count < 0 || in->box_count > 1000) return COMPANY_ALG_ERR_INVALID_INPUT;
      if (in->box_count > 0 && !in->boxes) return COMPANY_ALG_ERR_INVALID_INPUT;

      // 2. 乘法溢出与最大字节数安全防护
      if (!AdapterValidationHelper::CheckedMultiply("inputs[i].boxes", in->box_count,
                                                    sizeof(BoundingBox), 1024 * 1024)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // 3. COPY_IN 深拷贝
      std::vector<BoundingBoxDto> box_dtos;
      box_dtos.reserve(in->box_count);
      for (int b = 0; b < in->box_count; ++b) {
        box_dtos.push_back({in->boxes[b].x, in->boxes[b].y, in->boxes[b].w, in->boxes[b].h});
      }
    }
    return COMPANY_ALG_SUCCESS;
  }
};
```

---

## 4. 范式四：多级嵌套指针树 (Nested Pointer Tree)

适用场景：树状文档或对话结构（如 Document -> Sections -> Paragraphs）。

```cpp
typedef struct {
  const char* title;
  int paragraph_count;
  const char** paragraphs;  // 二级指针数组
} Section;

typedef struct {
  uint64_t request_id;
  int section_count;
  const Section* sections;  // 一级指针数组
} CompanyDocTreeInput;

// 适配器标准实现
class NestedPointerTreeAdapter : public IBusinessAdapter {
 public:
  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) const override {
    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyDocTreeInput*>(inputs[i]);
      if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

      if (in->section_count < 0 || in->section_count > 100) return COMPANY_ALG_ERR_INVALID_INPUT;
      if (in->section_count > 0 && !in->sections) return COMPANY_ALG_ERR_INVALID_INPUT;

      for (int s = 0; s < in->section_count; ++s) {
        const auto& sec = in->sections[s];
        if (sec.paragraph_count < 0 || sec.paragraph_count > 500) return COMPANY_ALG_ERR_INVALID_INPUT;
        if (sec.paragraph_count > 0 && !sec.paragraphs) return COMPANY_ALG_ERR_INVALID_INPUT;

        for (int p = 0; p < sec.paragraph_count; ++p) {
          if (!sec.paragraphs[p]) return COMPANY_ALG_ERR_INVALID_INPUT;
          // 深度安全解析与字符串拷贝...
        }
      }
    }
    return COMPANY_ALG_SUCCESS;
  }
};
```
