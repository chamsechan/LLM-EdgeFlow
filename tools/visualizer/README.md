# LLM-EdgeFlow Pipeline Studio

Pipeline Studio 是面向本地开发的 DAG 查看与编辑工具。终端视图、Web 工作台和自动化 CLI 共享 C++ Catalog 与 Validator，避免工具端维护另一套 Pipeline 规则。

## 前置条件

先从仓库根目录构建工具和 Demo：

```bash
cmake -B build -G Ninja -DLLM_EDGEFLOW_LINKER=auto
cmake --build build -j$(nproc)
```

## 终端查看

```bash
./build/alg_show configs/pipeline_doc_qa.json
```

该命令输出业务名、节点 ID、节点类型和依赖关系，不启动 Web 服务。

## Web 工作台

```bash
# 打开方案列表
./show --web

# 直接打开指定方案
./show configs/pipeline_dialogue_audit.json --web

# 使用自定义端口
./show --web --port 8081
```

服务只绑定 `127.0.0.1`。启动命令会尝试打开浏览器；无法自动打开时，按终端输出的地址访问即可。按 `Ctrl+C` 停止服务。

工作台支持：

- 打开、新建、编辑、校验和原子保存 `configs/pipeline_[a-z0-9_]+.json`。
- 使用 SHA-256 revision 检测 IDE 或 Git 引起的外部修改。
- 在 Graph 与 JSON 视图间同步，并将 Validator 诊断定位到相关节点。
- 在隔离临时目录中使用既有 Profile 运行未保存草稿。

节点坐标保存在浏览器 `localStorage`，不会写入 Pipeline JSON。工作台不提供账号或令牌鉴权，因此不得修改为对外网卡监听。

## 自动化 CLI

```bash
./build/alg_pipeline_tool catalog --business smart_doc_qa_v1
./build/alg_pipeline_tool describe-node TextEmbeddingNode
./build/alg_pipeline_tool validate configs/pipeline_doc_qa.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa.json
```

编排或修改 Pipeline 时，应先查询 Catalog 与节点 Definition，再执行 validate 和 plan。完整开发流程参见项目的 `pipeline-composer` skill 与[开发者指南](../../doc/developer_guide.md)。

## 安全边界

- 文件操作限制在仓库的 `configs/` 目录和受控文件名内，并拒绝符号链接与路径逃逸。
- 保存前必须通过 C++ Validator；正式文件使用同目录临时文件原子替换。
- 同一工作台最多运行一个草稿任务，可取消且有超时限制。
- 返回日志最多保留 2 MiB；草稿、临时配置和运行结果不会写入正式 `results/`。
