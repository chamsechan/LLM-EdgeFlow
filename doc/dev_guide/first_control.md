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

所有节点通过同一 Spec 声明 Control：普通字段使用 `WithControls`，复杂模板/规则使用
`WithControl` 声明 schema 和构建下一状态的函数。框架管理解析、writer 串行更新和不可变
快照；业务函数每次接收一份一致的参数，不需要覆写生命周期。完整生产例子见
[TextTemplateNode](../../src/common_nodes/text_template_node.cpp) 与
[TextRuleMatchNode](../../src/common_nodes/text_rule_match_node.cpp)。

## 2. 生成能直接编译的例子

先按根目录 [README](../../README.md#快速开始)完成快速开始构建；本练习无需模型权重。
以下命令都从仓库根目录执行，先确认命令 ID 尚未使用：

```bash
./build/alg_pipeline_tool catalog
./scripts/scaffold_custom_node.py PrefixControlNode --control-id 1001 --add-to-cmake --write-test
```

`1001` 是练习选择的 ID；若已被占用，选用另一个 ID 并同步下发值。标准 ID 保留给
框架；新 custom 命令在 1000..1999999999 按项目约定分配，2000000000 以上用于仓库测试，
发布后不复用。Catalog 会拒绝跨类型
重号。明确需要多个类型共享同义命令时，两端必须 `shared_id=true`，且 name、schema、
hot-swap 声明一致；通常直接复用同一份命令声明。重复使用同一个节点类型无需重复注册。

脚手架以[可编译模板](../../dev_support/node_authoring/starter_control_node.cpp)为唯一输入，
生成 `src/custom_nodes/prefix_control_node.cpp`。这个选项只生成 TextBatch → TextBatch、
1:1 保留来源的纯计算例子，不会改造任意已有 C++ 类。已有文件默认拒绝覆盖。
`--write-test` 生成独立测试文件；配合 `--add-to-cmake` 同时登记源文件和测试。

## 3. 阅读受控参数声明

| 声明点 | 作用 |
| --- | --- |
| `PrefixControlNodeParams` / `Field("prefix", ...)` | 声明业务参数结构体，绑定初值默认空字符串、字段说明与 64 字节业务校验 |
| `kUpdatePrefix` / `ReplaceFields(...)` | 声明具名命令 ID 与受控字段集合，自动投影 Control payload schema |
| `ApplyPrefix(...)` | 纯业务转换函数，接收普通数据与参数，无需接触锁或平台结构 |
| `WithControls(...)` | 将受控命令挂载到 Spec，框架自动管理不可变快照与并发更新事务 |

`WithControls` 引用 `ReplaceFields(kUpdatePrefix, "set_prefix", {"prefix"})`，框架复用 `Parameters` 已绑定的字段类型、默认值和业务校验规则自动生成 Control payload schema。初始配置写在节点的 `config`（例如 `{"prefix":"BASE:"}`），未设置时使用默认空字符串；Control 下发新值时通过相同校验规则验证，并通过不可变快照原子发布。非法初始配置会在预检拒绝，直接 Init 也返回具体原因。

框架采用 `ConfigurationSnapshot` 管理节点状态：更新在独立的 writer 锁内构建候选、校验成功后原子发布；正在执行的 Process 读取单次快照处理整批请求，互不干扰；更新失败保留旧配置。开发者只需关注普通参数绑定与业务逻辑，不需要手写互斥锁、JSON 解析或快照轮询。

模板展示值类型参数的控制。含模型句柄、外部资源的更新需要单独设计所有权；不能假设复制结构体就能深拷贝资源或撤销外部副作用。

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
  "deployment": {
    "io": {
      "io_binding": "keyword_match.operator.v1",
      "output_allocations": {
        "keyword_out": {
          "type": "keyword_out",
          "meta_num": 0,
          "metadata_type_id": 0,
          "capacities": {"match_result_json": 2047}
        }
      }
    }
  },
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
{"pipe_path": "pipeline.json"}
```

`pipe_path` 相对 `pipeline.conf` 所在目录解析；输入输出绑定和容量统一声明在
Pipeline 的 `deployment.io` 中。Demo 与直接调用 Operator 使用同一套部署解析规则。

`input.txt` 保存一行 `sample`；`control.json` 保存：

```json
{"prefix": "VIP:"}
```

```bash
./build/alg_pipeline_tool validate build/control_tutorial/pipeline.json
./build/alg_pipeline_tool plan build/control_tutorial/pipeline.json
./build/alg_demo --biz keyword_match --config build/control_tutorial/pipeline.conf --dataset build/control_tutorial/input.txt --control-cmd 1001 --control-file build/control_tutorial/control.json --output-dir build/control_tutorial/updated
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

Demo 的 `--control-cmd` 也可配置为 Profile 的 `control_cmd`，CLI 显式值优先；指定命令
必须提供 `control_file`。省略命令时保留该 Demo 的默认命令。Demo 默认不发送内置演示
更新；显式 `--example-control` 才启用，且显式文件优先。

同一 handle 的 Operator 调用串行；多个线程提交不保证顺序。内部直接调用
Pipeline/Node 的 Control 时，由调用者序列化更新。裸 payload 广播到所有声明支持该
命令的实例。一个 Pipeline 有多个同类节点时，用下面的信封只更新 `id: prefix`：

```json
{"$edgeflow_control":1,"node_id":"prefix","payload":{"prefix":"VIP:"}}
```

把该对象存入 Demo 的 Control 文件，或作为 `ControlJsonParam.json_param_str` 的 JSON
字符串；`cmd_id` 仍放在原参数中。`$edgeflow_control` 是保留标记；信封必须且只能含上述
三个字段，版本必须为整数 `1`，`node_id` 为非空的 Pipeline 实例 ID，`payload` 为对象。
Node 只收到内部 `payload`，无需编写路由代码。未知 ID、该实例不支持命令或 schema
校验失败会在调用 Node 前拒绝。

广播仍是尽力更新：任一节点语义失败可能已让其他节点生效；多次 Control 也不组成事务。
单节点应像模板一样先完成构造和校验，再替换配置。

交付使用[统一开发流程](../../CONTRIBUTING.md)和 `./scripts/run_all_tests.sh`。对已有
命令改变参数语义或公开接口时，先记录接口决策；普通新命令不用修改中央分发代码。
