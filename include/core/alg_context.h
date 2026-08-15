#pragma once

#include <any>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace alg_framework {

/**
 * @brief 请求级动态黑板上下文 (RequestContext)
 *
 * 特点:
 * 1. 类型擦除安全存储 (std::any)
 * 2. 自动生命周期托管，无裸指针内存泄露风险
 * 3. 支持全局错误码与错误信息传递
 */
class AlgContext {
 public:
  AlgContext() = default;
  ~AlgContext() = default;

  // 禁止拷贝，允许移动
  AlgContext(const AlgContext&) = delete;
  AlgContext& operator=(const AlgContext&) = delete;
  AlgContext(AlgContext&&) noexcept = default;
  AlgContext& operator=(AlgContext&&) noexcept = default;

  // 存入任意数据 (基础类型、STL 容器、自定义结构体、智能指针)
  template <typename T>
  void Set(const std::string& key, T&& value) {
    data_map_[key] = std::make_any<std::decay_t<T>>(std::forward<T>(value));
  }

  // 获取数据指针 (若不存在或类型不匹配则返回 nullptr)
  template <typename T>
  T* Get(const std::string& key) {
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return nullptr;
    return std::any_cast<T>(&(it->second));
  }

  // 常量获取
  template <typename T>
  const T* Get(const std::string& key) const {
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return nullptr;
    return std::any_cast<T>(&(it->second));
  }

  // 检查是否存在对应 key
  bool Has(const std::string& key) const {
    return data_map_.find(key) != data_map_.end();
  }

  // 清除指定 key
  void Erase(const std::string& key) { data_map_.erase(key); }

  // 清空所有上下文
  void Clear() {
    data_map_.clear();
    err_code_ = 0;
    err_msg_ = "OK";
  }

  // 设置错误状态
  void SetError(int err_code, const std::string& err_msg) {
    err_code_ = err_code;
    err_msg_ = err_msg;
  }

  int GetErrorCode() const { return err_code_; }
  const std::string& GetErrorMessage() const { return err_msg_; }
  bool IsOk() const { return err_code_ == 0; }

 private:
  std::unordered_map<std::string, std::any> data_map_;
  int err_code_ = 0;
  std::string err_msg_ = "OK";
};

}  // namespace alg_framework
