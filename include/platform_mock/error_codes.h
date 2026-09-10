#ifndef EDGEFLOW_PLATFORM_MOCK_ERROR_CODES_H_
#define EDGEFLOW_PLATFORM_MOCK_ERROR_CODES_H_

// Local platform mock declarations for this repository's Demo and tests.
// These are existing external-environment substitutes, not company SDK headers.

#define COMPANY_ALG_SUCCESS (0)              // 成功
#define COMPANY_ALG_ERR_INVALID_HANDLE (-1)  // 无效句柄 (nullptr 或野指针)
#define COMPANY_ALG_ERR_INVALID_PARAM \
  (-2)  // 参数非法 (如空字符串、缺少必填参数)
#define COMPANY_ALG_ERR_INVALID_INPUT \
  (-3)  // 输入数据非法 (空指针槽位、批大小超限或字段语义错误)
#define COMPANY_ALG_ERR_BUFFER_TOO_SMALL \
  (-4)  // 输出缓冲区不足或空槽位 (此时回填所需容量)
#define COMPANY_ALG_ERR_UNSUPPORTED_BIZ \
  (-5)  // 不支持或未注册的业务类型 / 业务配置不匹配
#define COMPANY_ALG_ERR_REGISTRY_CONFLICT (-6)  // 注册表冲突 (fail-closed 拦截)
#define COMPANY_ALG_ERR_UNSUPPORTED_CONTROL (-7)  // 不支持或未声明的控制命令
#define COMPANY_ALG_ERR_EXCEPTION (-99)  // 运行时捕获到 std::exception 异常
#define COMPANY_ALG_ERR_UNKNOWN (-100)  // 运行时捕获到未知异常

#endif  // EDGEFLOW_PLATFORM_MOCK_ERROR_CODES_H_
