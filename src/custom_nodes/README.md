# 自定义 Node

这里存放领域算法、特定前后处理和模型调用组合，与 `src/common_nodes/` 同属 Layer 3。
默认一个操作一个 `*_node.cpp`，直接放在本目录；文件名描述操作，不按项目或业务建立目录。
一个自定义 Node 可以被多个 Pipeline 使用，也可以与通用 Node 混合连线。

## 最短开发路径

1. 查询 `build/alg_pipeline_tool catalog --biz <biz_name>` 和 `describe-node`，优先复用
   已有操作；缺失的领域逻辑放在本目录，不要求先改造成通用算法。
2. 使用脚手架脚本快速生成合规骨架与测试（亦可手动编写）：
   ```bash
   # 生成纯算法处理节点（继承 NodeBase）
   ./scripts/scaffold_custom_node.py CustomFilterNode --kind compute --add-to-cmake --generate-test

   # 生成绑定单一模型能力的节点（继承 ModelBoundNode<ILlmModel>）
   ./scripts/scaffold_custom_node.py DomainPromptNode --kind model -m llm --add-to-cmake --generate-test

   # 生成严格 1:1 溯源推理节点（继承 TraceableUnaryInferenceNode）
   ./scripts/scaffold_custom_node.py FastAudioNode --kind unary_inference -m asr --add-to-cmake
   ```
3. 在生成的 `*_node.cpp` 中填充算法逻辑：
   - 纯处理继承 `NodeBase`；
   - 绑定单一模型继承 `ModelBoundNode<ILlmModel>`（可参考已落地的样例 [PromptGuidedLlmNode](prompt_guided_llm_node.cpp)）；
   - 一对一保序推理继承 `TraceableUnaryInferenceNode`。
   在 `ProcessNode` 中读取类型端口，完成前后处理并使用 `MakeTraceableItem(..., req_id, sub_id)` 保留溯源信息。
4. 在同一个源码文件声明完整 `NodeDefinition`，设置 `category = "custom"`，通过
   `REGISTER_NODE_WITH_DEFINITION` 注册。声明真实的端口、配置、模型绑定和并发能力；
   `biz_names` 只用于确有必要的业务契约限制，通常留空以供多个方案复用。
5. 将源码文件名加入本目录 [CMakeLists.txt](CMakeLists.txt) 的 `target_sources`（若脚手架未加 `--add-to-cmake`）。
   重新构建后通过 Catalog 查询；节点会沿用现有注册路径出现在 SDK、CLI 和 Studio 中，
   无需另写 UI 节点清单。
6. 在 `configs/` 中编排方案，运行 `validate` 和 `plan`；沿用已有外部契约时复用
   Adapter 与 Demo Profile。新输入输出结构按[业务接入指南](../../doc/BUSINESS_ONBOARDING.md)
   处理。测试放入现有 `tests/unit/nodes/` 或对应集成套件，交付流程见
   [CONTRIBUTING](../../CONTRIBUTING.md)。

可用能力始终以当前构建的 Catalog 为准。


## 复用与边界

- 通用与自定义 Node 使用同一套接口、端口和调度；`custom` 表示代码归属，不限制复用。
- 跨节点组合使用 Pipeline 连线，不直接构造或调用另一个 Node 绕过调度。重复的纯计算
  可提取为小型辅助函数；确有多个使用方时再整理共享代码，不预建每业务子目录。
- 平台 C ABI/Operator 结构和输入输出拷贝属于 `src/adapter/`；Node 不包含 Adapter
  头或业务 Blackboard key 头，只使用内部值与逻辑端口。
- Node 调用声明绑定的强类型模型能力，模型语义与厂商运行时分别由 Model/Backend 管理。
- 请求数据只保存在局部变量或 `AlgContext`；成员只存配置或安全共享句柄。
- 通用 Node、Core 和 Model/Backend 不依赖这里的实现。自定义 Node 经实际复用与评审
  后可以提升到 `common_nodes/`，迁移时保留节点类型名和端口契约以避免破坏已有方案。
