# Node 作者包装性能探针

```bash
python3 dev_support/node_authoring/benchmark/run.py \
  --source "$PWD" --build /tmp/edgeflow-authoring-benchmark
```

使用仓库固定版本的本地依赖缓存，不下载模型。所有生成源码、缓存链接与原始结果写入
所选 build 旁的 `*-source` 目录；不会把历史源码复制进当前生产注册。

- 旧 LLM starter 从 Git `87f490b` 提取，只改类型/注册名避免重名；新旧包装链接同一当前 runtime。
- 旧 starter 所需的两个已退役辅助头文件固定从 Git `87a28b7` 提取到工作目录的
  `baseline_include/nodes`，仅作为探针私有比较基线；当前 SDK 不恢复旧作者接口。
  `summary.json` 的 `baseline_helper_revision` 记录该版本。
- 旧 Map 为等价显式 `NodeBase` 透传实现的重建，不冒充历史生产 Node。
- 每批 32 条、每条 1024 字节，固定 Echo mock；预热 100 次，每个进程测 10000 次，独立重复 5 次。
- 测量 `Process` 的线程 CPU 时间及 C++ `new/new[]` 请求次数、字节数；不包含输入发布和输出断言，
  也不代表全部 malloc、存活堆或峰值 RSS。每次校验文本及 req/sub，记录实际模型调用次数。
- `new_batch` 展示作者使用两次 `MapPayloads` 的自由逻辑；`new_batch_inplace` 原地格式化 owned 输出，
  与旧 starter 使用同一分配策略。两者差异用于区分作者算法和框架包装成本。
- 两份 starter 翻译单元各独立编译 5 次，使用相同 Release 参数、不链接；不是整个工程构建耗时。

[2026-09-13 记录](results_2026-09-13.json)保存本机实测。此探针不包含真实模型延时，
不证明目标设备性能或真实开发者体验。RFC-0052 记录验收范围与未覆盖事项。
