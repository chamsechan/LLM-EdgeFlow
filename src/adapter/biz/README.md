# Business IO Bindings & Exposures (业务接入绑定与契约暴露)

本目录包含各个业务场景的 `BizDefinition` 注册、生产接入暴露声明 (`REGISTER_BIZ_EXPOSURE`) 以及显式 I/O 绑定 (`REGISTER_IO_BINDING`)。

## 规范与契约
- 每个业务绑定文件独立声明该业务的内部 Blackboard 契约。
- 显式声明该业务支持的 Operator 绑定，将独立输入/输出转换器与业务逻辑端口映射起来。
- 业务配置通过 Pipeline 的 `deployment.io.io_binding` 显式选择全局唯一的 `binding_id`。
- 同名端口用 `BindIoPort(port)`，非同名用 `BindIoPort(logical_port, actual_key)`；复用转换器声明的名字和类型，不重复手写字符串对。
- 保留独立转换器、生产曝光及完整业务 ingress/egress；不能仅根据 converter 读写集合推导全部业务契约。
