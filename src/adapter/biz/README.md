# Business IO Bindings & Exposures (业务接入绑定与契约暴露)

本目录包含各个业务场景的 `BizDefinition` 注册、生产接入暴露声明 (`REGISTER_BIZ_EXPOSURE`) 以及显式 I/O 绑定 (`REGISTER_IO_BINDING`)。

## 规范与契约
- 每个业务绑定文件独立声明该业务的内部 Blackboard 契约。
- 显式声明该业务支持的 C ABI 绑定与 Operator 绑定，将独立输入/输出转换器与业务逻辑端口映射起来。
- 业务配置通过 `data.io_binding` 显式选择全局唯一的 `binding_id`。
