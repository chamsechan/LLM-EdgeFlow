# 第一个 Control：给文本节点增加运行时前缀更新

本练习面向能编写普通 C++ 的开发者。完成后，原本不命中的文本 `sample` 会因运行时
下发前缀 `VIP:` 而命中已有规则；你不需要修改 Pipeline 调度或 Operator 分发代码。

## 1. 文件与职责

| 要做什么 | 修改位置 |
| --- | --- |
| 参数声明、业务校验和更新逻辑 | 所属 `src/custom_nodes/<操作>_node.cpp` |
| 新节点登记编译 | 同目录 CMakeLists；脚手架可自动完成 |
| 控制命令的行为断言 | 已有节点测试套件 |
| 新平台专有结构的转换和拷贝 | Integration；普通 JSON Control 使用已有通用入口 |

已有节点增加命令时，只需移植下面模板中的声明和 `ControlNode` 片段。继续使用原有
`NodeBase` / `ModelBoundNode` 等基类，保持端口、模型绑定和请求逻辑。一个节点支持多个
命令时，可在 `ControlNode` 中分支调用普通成员函数，每个函数只处理一种更新。

## 2. 生成能直接编译的例子

先按根目录 [README](../../README.md#快速开始)完成默认构建；本练习无需模型权重。
以下命令都从仓库根目录执行，先确认命令 ID 尚未使用：

```bash
./build/alg_pipeline_tool catalog
./scripts/scaffold_custom_node.py PrefixControlNode --control-id 1001 --add-to-cmake --generate-test
```

`1001` 是练习选择的 ID；若已被占用，选用另一个 ID 并同步下发值。标准 ID 保留给
框架；新 custom 命令在 1000..1999999999 按项目约定分配，2000000000 以上用于仓库测试，
发布后不复用。Catalog 会拒绝跨类型
重号。明确需要多个类型共享同义命令时，两端必须 `shared_id=true`，且 name、schema、
hot-swap 声明一致；通常直接复用同一份命令声明。重复使用同一个节点类型无需重复注册。

脚手架以[可编译模板](../../dev_support/node_authoring/starter_control_node.cpp)为唯一输入，
生成 `src/custom_nodes/prefix_control_node.cpp`。这个选项只生成 TextBatch → TextBatch、
1:1 保留来源的纯计算例子，不会改造任意已有 C++ 类。已有文件默认拒绝覆盖。
`--generate-test` 打印注册及业务测试代码，请将它加入现有套件；不会自动修改测试文件。

## 3. 阅读三个编辑点

| 编辑点 | 作用 |
| --- | --- |
| `kUpdatePrefix` / `PrefixCommand()` | 同文件的具名 ID、命令说明和参数 schema |
| `ControlNode()` | 解析到拥有数据的 JSON 值，构造新前缀，进行 64 字节限制的业务校验 |
| `ProcessNode()` | 为整批请求读取一次前缀，在保留 `(req_id, sub_id)` 的输出中使用它 |

`def.control_commands` 引用 `PrefixCommand()`；`ParseControlPayload` 也引用其中的
schema，避免重复维护字段检查。现有校验子集包括 `type`、`enum`、`required`、
`properties`、`minProperties`、`additionalProperties` 和同类型 `items`。只使用这些
关键字；字符串长度、字段关系、规则编译等约束使用普通 C++ 语义校验。它不是完整的
JSON Schema 实现。

参数校验完成前不修改在线配置。模板先构造拥有数据的 `std::string next`，再在写锁中
`swap`；失败保留旧配置。输入平台字符串仅借用到同步调用返回，Integration 和节点
负责形成各自拥有的内部值。不要把平台指针、请求 Context 或本次输入存进节点成员。

模板只展示值类型配置。含模型句柄、外部资源的更新需要单独设计所有权；不能假设复制
结构体就能深拷贝资源或撤销外部副作用。

## 4. 编译并检查实际注册

```bash
cmake --build build --target alg_sdk alg_pipeline_tool alg_demo -j 4
./build/alg_pipeline_tool describe-node PrefixControlNode
```

输出应包含 `cmd_id: 1001`、`name: set_prefix` 和必填字符串 `prefix`。生产 Catalog
只在你生成并登记源码后才增加节点；模板本身不作为内置能力交付。

## 5. 通过已有 Demo 下发

创建练习目录：

```bash
mkdir -p build/control_tutorial
```

保存以下文件为 `build/control_tutorial/pipeline.json`：

```json
{
  "biz_name": "keyword_match_v1",
  "models": [],
  "pipeline": [
    {
      "id": "prefix",
      "node_type": "PrefixControlNode",
      "depends_on": [],
      "ports": {
        "inputs": {"input": "input_sentences"},
        "outputs": {"output": "prefixed"}
      }
    },
    {
      "id": "matcher",
      "node_type": "TextRuleMatchNode",
      "depends_on": ["prefix"],
      "ports": {
        "inputs": {"text": "prefixed"},
        "outputs": {"matches": "rule_matches"}
      },
      "config": {"categories": {"PREFIX_APPLIED": ["VIP:sample"]}}
    }
  ]
}
```

同目录保存 `pipeline.conf`：

```json
{
  "data": {
    "pipe_path": "build/control_tutorial/pipeline.json",
    "mem_que": {
      "type": "keyword_out",
      "meta_num": 0,
      "metadata_type_id": 0,
      "capacities": {"match_result_json": 2047}
    }
  }
}
```

`pipe_path` 相对部署根解析。本文从仓库根目录传入相对路径
`--config build/control_tutorial/pipeline.conf`，Demo 据此使用仓库根作为部署根；
因此这里填写 `build/control_tutorial/pipeline.json`，而不是只写文件名。
宿主直接调用 Operator 时，部署根由 Create 的 `model_path` 指定。

`input.txt` 保存一行 `sample`；`control.json` 保存：

```json
{"prefix": "VIP:"}
```

```bash
./build/alg_pipeline_tool validate build/control_tutorial/pipeline.json
./build/alg_pipeline_tool plan build/control_tutorial/pipeline.json
./build/alg_demo --biz keyword_match --config build/control_tutorial/pipeline.conf --dataset build/control_tutorial/input.txt --no-default-control --control-cmd 1001 --control-file build/control_tutorial/control.json --output-dir build/control_tutorial/updated
```

查看 `build/control_tutorial/updated/keyword_match/results.jsonl`：应有 `status: 0`、
`is_hit: true`，匹配类别为 `PREFIX_APPLIED`。去掉 `--control-cmd` 与 `--control-file` 后
使用另一个输出目录运行，应得到 `is_hit: false`。这证明新增命令在节点中实际生效。

将前缀改为数字或超过 64 字节的字符串时，Demo 应退出 5 并报告节点实例和具体原因。
脚手架生成的节点测试还会在同一实例上检查失败后旧前缀仍生效。

## 6. 实际宿主调用与边界

Operator 调用方式为：

```cpp
ControlJsonParam param{1001, R"({"prefix":"VIP:"})"};
int ret = ops.Control(handle, ControlCommand::kJson, &param);
// ret != 0 时，立即读取同线程 GetOperatorLastError()。
```

`kJson` 选择唯一参数结构；节点命令 ID 位于 `param.cmd_id`。payload 必须是非空 JSON object，
UTF-8 字节数小于 65536，不含终止符。已有 Operator 命令 1/2/3 仍可按原结构调用。
C ABI 继续直接使用 `CompanyAlgParamControl{cmd_id, json}`，不需要新增导出函数。

Demo 的 `--control-cmd` 也可配置为 Profile 的 `control_cmd`，CLI 显式值优先；指定命令
必须提供 `control_file`。省略命令时保留该 Demo 的默认命令。`--no-default-control`
只关闭默认演示更新，显式文件仍执行。

同一 handle 的 C ABI / Operator 调用串行；多个线程提交不保证顺序。内部直接调用
Pipeline/Node 的 Control 时，由调用者序列化更新。一次命令广播到所有声明支持它的
实例，任一节点语义失败可能已让其他节点生效；多次 Control 也不组成事务。需要精确
实例寻址或成套切换时应先提出独立需求。

交付使用[统一开发流程](../../CONTRIBUTING.md)和 `./scripts/run_all_tests.sh`。对已有
命令改变参数语义或公开接口时，先记录兼容决策；普通新命令不用修改中央分发代码。
