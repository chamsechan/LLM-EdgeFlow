#pragma once

#include <any>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/blackboard_key.h"

namespace alg_framework {

/**
 * @brief 请求级动态黑板上下文 (RequestContext)
 *
 * 特点:
 * 1. 类型擦除的不可变快照存储 (std::any)
 * 2. Read 返回的视图在本 AlgContext 析构前保持有效且内容稳定
 * 3. Publish 对新 key 执行单次发布
 * 4. Set/Get/Erase/Clear 保留为迁移兼容接口
 * 5. 支持全局错误码与错误信息传递
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
    auto snapshot = MakeSnapshot(std::forward<T>(value));
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
   * 返回指针在本 AlgContext 析构前保持有效。后续兼容 Set、Erase 或 Clear
   * 不会改变该指针指向的旧快照。
   */
  template <typename T>
  const T* Read(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return nullptr;
    return std::any_cast<T>(it->second.get());
  }

  template <typename T>
  const T* Read(const BlackboardKey<T>& key) const {
    return Read<T>(key.name);
  }

  // 迁移兼容：允许替换同名 key；旧快照延迟到 AlgContext 析构时释放。
  template <typename T>
  void Set(const std::string& key, T&& value) {
    auto snapshot = MakeSnapshot(std::forward<T>(value));
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = data_map_.find(key);
    if (it == data_map_.end()) {
      data_map_.emplace(key, std::move(snapshot));
      return;
    }
    Retire(it->second);
    it->second = std::move(snapshot);
  }

  template <typename T, typename U>
  void Set(const BlackboardKey<T>& key, U&& value) {
    static_assert(std::is_same_v<T, std::decay_t<U>>,
                  "BlackboardKey<T> only accepts values of T");
    Set(key.name, std::forward<U>(value));
  }

  // 迁移兼容：Get 是 Read 的只读别名，不再暴露可变裸指针。
  template <typename T>
  const T* Get(const std::string& key) {
    return Read<T>(key);
  }

  template <typename T>
  const T* Get(const BlackboardKey<T>& key) {
    return Read(key);
  }

  template <typename T>
  const T* Get(const std::string& key) const {
    return Read<T>(key);
  }

  template <typename T>
  const T* Get(const BlackboardKey<T>& key) const {
    return Read(key);
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
    auto it = data_map_.find(key);
    if (it == data_map_.end()) return;
    Retire(it->second);
    data_map_.erase(it);
  }

  template <typename T>
  void Erase(const BlackboardKey<T>& key) {
    Erase(key.name);
  }

  // 清空所有上下文
  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    retired_snapshots_.reserve(retired_snapshots_.size() + data_map_.size());
    for (const auto& entry : data_map_) {
      Retire(entry.second);
    }
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
  using Snapshot = std::shared_ptr<const std::any>;

  template <typename T>
  static Snapshot MakeSnapshot(T&& value) {
    return std::make_shared<const std::any>(
        std::make_any<std::decay_t<T>>(std::forward<T>(value)));
  }

  void Retire(const Snapshot& snapshot) {
    retired_snapshots_.push_back(snapshot);
  }

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, Snapshot> data_map_;
  // 兼容替换/删除后保留旧快照，确保已经返回的只读指针不悬空。
  std::vector<Snapshot> retired_snapshots_;
  int err_code_ = 0;
  std::string err_msg_ = "OK";
};

}  // namespace alg_framework
