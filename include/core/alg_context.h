#pragma once

#include <any>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "core/blackboard_key.h"

namespace llm_edgeflow {

class Pipeline;

/**
 * @brief 请求级动态黑板上下文 (RequestContext)
 *
 * 特点:
 * 1. 类型擦除的不可变快照存储 (std::any)
 * 2. Read 返回的视图在本 AlgContext 析构前保持有效且内容稳定
 * 3. Publish 对新 key 执行单次发布
 * 4. 支持最终错误状态，并为并行 Node 保留线程内诊断快照
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

  /**
   * @brief 单次发布新 key。
   * @return key 不存在并成功发布时返回 true；重复 key 返回 false 且保留原值。
   */
  template <typename T>
  bool Publish(const std::string& key, T&& value) {
    auto snapshot = std::make_any<std::decay_t<T>>(std::forward<T>(value));
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return data_map_.emplace(key, std::move(snapshot)).second;
  }

  template <typename T, typename U>
  bool Publish(const BlackboardKey<T>& key, U&& value) {
    static_assert(std::is_same_v<T, std::decay_t<U>>,
                  "BlackboardKey<T> only accepts values of T");
    return Publish(key.name, std::forward<U>(value));
  }

  /**
   * @brief 获取不可变快照视图。
   *
   * 返回指针在本 AlgContext 析构前保持有效。Publish 不覆盖或删除已有值。
   */
  template <typename T>
  const T* Read(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return nullptr;
    return std::any_cast<T>(&it->second);
  }

  template <typename T>
  const T* Read(const BlackboardKey<T>& key) const {
    return Read<T>(key.name);
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

  // 设置最终错误状态，同时记录当前执行线程的诊断供 Pipeline 隔离收集。
  void SetError(int err_code, const std::string& err_msg) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    err_code_ = err_code;
    err_msg_ = err_msg;
    thread_errors_[std::this_thread::get_id()] = {err_code, err_msg};
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
  struct ErrorState {
    int code = 0;
    std::string message;
  };

  ErrorState TakeCurrentThreadError() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = thread_errors_.find(std::this_thread::get_id());
    if (it == thread_errors_.end()) return {};
    ErrorState result = std::move(it->second);
    thread_errors_.erase(it);
    return result;
  }

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::any> data_map_;
  int err_code_ = 0;
  std::string err_msg_ = "OK";
  std::unordered_map<std::thread::id, ErrorState> thread_errors_;

  friend class Pipeline;
};

}  // namespace llm_edgeflow
