# RFC 0034: Kite 原生能力在现有业务中的完整接入

- **RFC 编号**：0034-kitellm-capability-coverage
- **创建日期**：2026-09-05
- **文档状态**：Completed
- **关联分支**：`feat/kite-capability-coverage`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team

## 1. 背景与范围

在 RFC-0033 的设备适配基础上，覆盖当前同步业务契约可消费的 Kite 原生能力。
核对固定 v0.1.0 提交 `5e58820f39919fce2046e2fd703c62601a5df59b` 的
[视觉说明](https://github.com/chamsechan/kiteLLM/blob/5e58820f39919fce2046e2fd703c62601a5df59b/doc/vision.md)
及 C API/实现：文本生成和原始 RGB 图像＋文本生成已实现；ASR 输入、Embedding、
专用 Rerank 输出不在该接口支持范围。不同步扩张到当前应用没有的异步/流式公开契约。

## 2. 分层设计

- Layer 1～3：沿用 Operator/C ABI、PipelineValidator、OcrDetectNode、LlmGenerateNode
  及其端口。业务行为仅通过配置组合。
- Layer 4：新增中性 image_text_generation 协议，载荷为文本和已解码的 RGB 平面图像
  （尺寸及模型 patch 大小），不泄漏 Kite 类型。Kite Backend 映射到原生多模态聊天输入。
- 新增 vision_document OCR Model，拥有图像解码、尺寸限制、补边、识别提示和结果解释；
  使用 IOcrModel 返回 combined_text，boxes 留空，不能伪造定位/置信度。
- libmtmd 自行管理视觉投影与模型聊天模板；Model 提供业务中立的识别指令，Backend
  只转换中性图像/文本请求与原生参数，不包含票据业务逻辑。
- 图像解码依赖 stb_image v2.30，固定提交及 SHA-256，通过 cmake 下载到忽略缓存，
  不捆绑源码。仅 Model 图像工具包含解码头。
- vision.mmproj 使用模型目录内的相对路径并在加载前校验，不允许配置路径逃逸；
  其他运行参数仍由原生 SDK 校验。

## 3. 配置与兼容性

新增独立 Kite profile 文件及文本/混合/OCR 部署配置，保持默认 profile 套件的原有行为。Demo CLI 新增通用 --profiles-file 参数选择部署文件。
文本仍为 qwen_causal_lm；OCR 使用匹配的视觉 GGUF/mmproj，之后交由文本模型结构化。
复用已有业务名，不引入 Kite 专用业务分支。Kite 支持两个独立协议，加载会话的协议
必须与 Model 要求一致；无视觉投影的配置不能创建图像协议会话。

## 4. 验证与边界

扩展现有 engine、模型、catalog 和 demo 测试套件：协议不匹配、空/非法图像、尺寸限制、
补边/CHW 转换、失败无部分输出、req_id/sub_id、原生真实视觉生成与已有文本回归。
下载固定模型并校验摘要；实体抽取、问答（含精排）、审核和 OCR 使用真实模型运行。
视觉 smoke 证明图像编码与业务链路，识别准确率另据具体模型评估，不以非空结果冒充
票据字段准确率验收。ASR 等不支持能力保持明确说明。

交付运行默认 ./scripts/run_all_tests.sh，并运行 Kite 专项真实模型门禁及独立 Demo。

## 5. 实施记录

- [x] 上游接口与当前 Catalog 核对。
- [x] 中性图像协议、Backend、OCR Model 和注册。
- [x] 独立 Kite 配置、模型下载与真实业务验证。
- [x] 默认回归、分层检查、文档更新。


## 6. 验收记录

- 本地 Linux aarch64，使用固定 Kite v0.1.0 发布包、真实 Qwen2.5 0.5B GGUF、
  BGE/重排 ONNX 和 SmolVLM-256M + 配套视觉投影，所有下载按固定 SHA-256 验证。
- 默认 `LLM_EDGEFLOW_JOBS=8 ./scripts/run_all_tests.sh` 最终 88/88 CTest 通过。
- Kite 构建启用文本/视觉真实测试与 `LLM_EDGEFLOW_TEST_KITELLM_DEMOS=1`：
  完整 88/88 CTest 通过，7 个独立部署 Profile 的 11 条样本均 status=0。
  真实图像测试通过；默认构建中的条件跳过不计为这项验收。
- 五份新增 Pipeline 均通过生产 alg_pipeline_tool 的 validate 和 plan。
- 独立 alg_demo 验证了通用 --profiles-file 选择、文本和 OCR 路径。
  示例图像文字为 INVOICE / Number 12345 / TOTAL 12.50，最终结构化结果为
  `{"invoice_number":"12345","total":"12.50"}`，检测框数量为 0。
- Kite 审核提示补齐校验要求的 risk_score；问答生成上限保持 128 tokens，与现有
  固定 C ABI 回答缓冲区兼容。保留严格解析/容量错误，不用兜底结果掩盖失败。
- 能力验收不等同于业务准确率验收。小模型在审核样本中存在误判，不能把生成结果
  当成可靠的自动审核决策；OCR 仅验证了示例图像，不代表通用票据准确率。
- 既有私有 Kite CI 已配置视觉模型、混合业务与 Catalog/规划检查；本次未上传，
  未执行远程 CI，也未验证 x64/GPU/NPU 硬件。
