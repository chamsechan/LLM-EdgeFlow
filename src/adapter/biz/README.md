# Business IO Bindings (业务契约与接入绑定)

本目录包含各个业务场景的 `BizDefinition` 注册以及显式 I/O 绑定 (`REGISTER_IO_BINDING`)。

## 规范与契约
- 每个业务绑定文件通过 `BizDefinition` 声明业务名、Demo 名及完整 ingress/egress Blackboard 契约，并调用 `PipelineCatalog::RegisterBizDefinition` 登记。
- 显式声明该业务支持的 Operator 绑定，选择独立的输入/输出转换器，绑定批次上限默认为框架标准值 64，只有实测确需更小值时才覆盖 `max_batch_size`；转换器只在自身确有限制时才声明上限。
- 业务配置通过 Pipeline 的 `deployment.io.io_binding` 显式选择全局唯一的 `binding_id`。
- 转换器的逻辑端口默认映射到同名 Blackboard Key；只有非同名映射才写 `BindIoPort(logical_port, actual_key)`，复用转换器声明的名字和类型，不重复手写字符串对。
- 保留独立转换器及完整业务 ingress/egress；不能仅根据 converter 读写集合推导全部业务契约。
