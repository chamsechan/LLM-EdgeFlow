#pragma once

#include <any>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "core/blackboard_key.h"

namespace alg_framework {

/**
 * @brief 请求级动态黑板上下文 (RequestContext)
 *
 * 特点:
 * 1. 类型擦除安全存储 (std::any)
 * 2. 线程安全并发读写保护 (std::shared_mutex: 读读不互斥、写写互斥)
 * 3. 自动生命周期托管，无裸指针内存泄露风险
 * 4. 支持全局错误码与错误信息传递
 */
class AlgContext {
 public:
  AlgContext() = default;
  ~AlgContext() = default;

  // 禁止拷贝与移动 (持有多线程锁变量)
  AlgContext(const AlgContext&) = delete;
  AlgContext& operator=(const AlgContext&) = delete;
  AlgContext(AlgContext&&) = delete;
  AlgContext& operator=(AlgContext&&) = delete;

  // 存入任意数据 (基础类型、STL 容器、自定义结构体、智能指针)
  template <typename T>
  void Set(const std::string& key, T&& value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_map_[key] = std::make_any<std::decay_t<T>>(std::forward<T>(value));
  }

  template <typename T, typename U>
  void Set(const BlackboardKey<T>& key, U&& value) {
    static_assert(std::is_same_v<T, std::decay_t<U>>,
                  "BlackboardKey<T> only accepts values of T");
    Set(key.name, std::forward<U>(value));
  }

  // 获取数据指针 (若不存在或类型不匹配则返回 nullptr)
  template <typename T>
  T* Get(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return nullptr;
    return std::any_cast<T>(&(it->second));
  }

  template <typename T>
  T* Get(const BlackboardKey<T>& key) {
    return Get<T>(key.name);
  }

  // 常量获取
  template <typename T>
  const T* Get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return nullptr;
    return std::any_cast<T>(&(it->second));
  }

  template <typename T>
  const T* Get(const BlackboardKey<T>& key) const {
    return Get<T>(key.name);
  }

  // 检查是否存在对应 key
  bool Has(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_map_.find(key) != data_map_.end();
  }

  template <typename T>
  bool Has(const BlackboardKey<T>& key) const {
    return Has(key.name);
  }

  // 清除指定 key
  void Erase(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_map_.erase(key);
  }

  template <typename T>
  void Erase(const BlackboardKey<T>& key) {
    Erase(key.name);
  }

  // 清空所有上下文
  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_map_.clear();
    err_code_ = 0;
    err_msg_ = "OK";
  }

  // 设置错误状态
  void SetError(int err_code, const std::string& err_msg) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    err_code_ = err_code;
    err_msg_ = err_msg;
  }

  int GetErrorCode() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return err_code_;
  }

  std::string GetErrorMessage() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return err_msg_;
  }

  bool IsOk() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return err_code_ == 0;
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::any> data_map_;
  int err_code_ = 0;
  std::string err_msg_ = "OK";
};

}  // namespace alg_framework
