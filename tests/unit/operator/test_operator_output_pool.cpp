#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "adapter/operator/operator_output_pool.h"
#include "adapter/operator/operator_process_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "scoped_allocation_failure.h"

namespace llm_edgeflow {

class OperatorOutputPoolTest : public ::testing::Test {
 protected:
  void SetUp() override { OperatorValueTypeRegistry::Instance().GlobalInit(); }
};

// 1. 深度 0 归一化为 25 且正常预分配，深度 > 1024 拦截
TEST_F(OperatorOutputPoolTest, DepthZeroNormalizedTo25AndMaxLimitChecked) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  int ret =
      OutputPoolState::Create("keyword_out", 0, spec, binding, &pool, &err);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(pool->Depth(), 25u);
  EXPECT_EQ(pool->FreeBlockCount(), 25u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // 超过 1024 硬上限
  std::shared_ptr<OutputPoolState> pool_bad;
  ret = OutputPoolState::Create("keyword_out", 1025, spec, binding, &pool_bad,
                                &err);
  EXPECT_EQ(ret, -2);
  EXPECT_EQ(pool_bad, nullptr);
}

// 2. 状态账本防重复归还、防外部指针注入与无下溢
TEST_F(OperatorOutputPoolTest, LedgerPreservesFifoAndRejectsInvalidReturns) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 3, spec, binding, &pool, &err), 0);
  ASSERT_EQ(pool->FreeBlockCount(), 3u);

  void* block1 = nullptr;
  ASSERT_EQ(pool->Acquire(&block1), 0);
  ASSERT_NE(block1, nullptr);
  EXPECT_EQ(pool->FreeBlockCount(), 2u);
  EXPECT_EQ(pool->CheckedOutCount(), 1u);

  // 正常归还 1 次
  pool->ReturnBlock(block1);
  EXPECT_EQ(pool->FreeBlockCount(), 3u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // 重复归还 block1 -> 应被账本拦截，不增加 free 数量，计数不下溢
  pool->ReturnBlock(block1);
  EXPECT_EQ(pool->FreeBlockCount(), 3u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // 归还非本池分配的野指针 -> 应被账本拦截
  int dummy = 123;
  pool->ReturnBlock(&dummy);
  EXPECT_EQ(pool->FreeBlockCount(), 3u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);

  // Leave block1 queued; return the other two out of checkout order.
  void *block2 = nullptr, *block3 = nullptr;
  ASSERT_EQ(pool->Acquire(&block2), 0);
  ASSERT_EQ(pool->Acquire(&block3), 0);
  EXPECT_NE(block2, block1);
  EXPECT_NE(block3, block1);
  EXPECT_NE(block2, block3);
  pool->ReturnBlock(block3);
  pool->ReturnBlock(block2);
  // Repeated rotations preserve FIFO addresses without allocating.
  for (int round = 0; round < 3; ++round) {
    for (void* expected : {block1, block3, block2}) {
      ASSERT_GT(pool->FreeBlockCount(), 0u);
      void* actual = nullptr;
      int result = 0;
      bool allocated = false;
      {
        test_support::ScopedAllocationFailure failure(0);
        result = pool->Acquire(&actual);
        pool->ReturnBlock(actual);
        allocated = failure.Triggered();
      }
      ASSERT_EQ(result, 0);
      EXPECT_EQ(actual, expected);
      EXPECT_FALSE(allocated);
    }
  }
  EXPECT_EQ(pool->FreeBlockCount(), 3u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);
}

// 3. 地址复用与 Reset 契约保留嵌套容量
TEST_F(OperatorOutputPoolTest, AddressReuseAndResetContract) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";
  spec.capacities["match_result_json"] = 100;

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 1, spec, binding, &pool, &err), 0);

  void* b1 = nullptr;
  ASSERT_EQ(pool->Acquire(&b1), 0);
  auto* out1 = static_cast<CompanyOperatorKeywordOutput*>(b1);
  out1->request_id = 999;
  out1->is_hit = 1;
  out1->status_code = -42;
  std::strcpy(out1->match_result_json->data, "hello world");
  out1->match_result_json->length = 11;
  char* original_data_addr = out1->match_result_json->data;

  // 归还
  pool->ReturnBlock(b1);

  // 再次检出
  void* b2 = nullptr;
  ASSERT_EQ(pool->Acquire(&b2), 0);
  EXPECT_EQ(b1, b2);
  auto* out2 = static_cast<CompanyOperatorKeywordOutput*>(b2);

  // 验证 Reset 契约：内容重置为初始，但物理地址和容量完好保留
  EXPECT_EQ(out2->request_id, 0u);
  EXPECT_EQ(out2->is_hit, 0);
  EXPECT_EQ(out2->status_code, 0);
  EXPECT_EQ(out2->match_result_json->length, 0);
  EXPECT_EQ(out2->match_result_json->data[0], '\0');
  EXPECT_EQ(out2->match_result_json->data, original_data_addr);

  pool->ReturnBlock(b2);
}

TEST_F(OperatorOutputPoolTest,
       EntityOutputReusePreservesDefaultAndCustomCapacity) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("entity_out");
  ASSERT_NE(binding, nullptr);
  for (uint32_t capacity : {2047u, 100u}) {
    SCOPED_TRACE(capacity);
    ResolvedOutputPoolSpec spec;
    spec.type = "entity_out";
    if (capacity != 2047u) spec.capacities["entities_json"] = capacity;
    std::shared_ptr<OutputPoolState> pool;
    std::string err;
    ASSERT_EQ(OutputPoolState::Create(spec.type, 1, spec, binding, &pool, &err),
              0)
        << err;
    void* block = nullptr;
    ASSERT_EQ(pool->Acquire(&block), 0);
    auto* out = static_cast<CompanyOperatorEntityOutput*>(block);
    EXPECT_EQ(out->request_id, 0u);
    EXPECT_EQ(out->status_code, 0);
    ASSERT_NE(out->entities_json, nullptr);
    CompanyString* original_string = out->entities_json;
    char* original_data = original_string->data;
    ASSERT_NE(original_data, nullptr);
    EXPECT_EQ(original_string->length, 0);
    EXPECT_EQ(original_data[0], '\0');
    out->request_id = 999;
    out->status_code = -42;
    std::memset(original_data, 'x', capacity);
    original_data[capacity] = '\0';
    original_string->length = static_cast<int32_t>(capacity);

    void* reused = nullptr;
    int result = -1;
    bool allocated = false;
    {
      test_support::ScopedAllocationFailure failure(0);
      pool->ReturnBlock(block);
      result = pool->Acquire(&reused);
      allocated = failure.Triggered();
    }
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(allocated);
    EXPECT_EQ(reused, block);
    out = static_cast<CompanyOperatorEntityOutput*>(reused);
    EXPECT_EQ(out->request_id, 0u);
    EXPECT_EQ(out->status_code, 0);
    EXPECT_EQ(out->entities_json, original_string);
    EXPECT_EQ(out->entities_json->data, original_data);
    EXPECT_EQ(out->entities_json->length, 0);
    EXPECT_EQ(original_data[0], '\0');
    pool->ReturnBlock(reused);
  }
}

namespace {

template <typename T, typename Mutate, typename Verify>
void CheckMultiStringOutputReuse(const char* suffix,
                                 std::vector<CompanyString * T::*> fields,
                                 Mutate mutate, Verify verify) {
  SCOPED_TRACE(suffix);
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(suffix);
  ASSERT_NE(binding, nullptr);
  ResolvedOutputPoolSpec spec;
  spec.type = suffix;
  for (const auto& [name, config] :
       binding->output_layout.string_capacity_fields) {
    spec.capacities[name] = 17;
  }
  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(OutputPoolState::Create(suffix, 1, spec, binding, &pool, &err), 0)
      << err;
  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  auto* out = static_cast<T*>(block);
  verify(*out);
  mutate(*out);
  std::vector<CompanyString*> strings;
  std::vector<char*> buffers;
  for (auto member : fields) {
    auto* str = out->*member;
    ASSERT_NE(str, nullptr);
    ASSERT_NE(str->data, nullptr);
    EXPECT_EQ(str->length, 0);
    EXPECT_EQ(str->data[0], '\0');
    strings.push_back(str);
    buffers.push_back(str->data);
    std::memset(str->data, 'x', 17);
    str->data[17] = '\0';
    str->length = 17;
  }
  void* reused = nullptr;
  int result = -1;
  bool allocated = false;
  {
    test_support::ScopedAllocationFailure failure(0);
    pool->ReturnBlock(block);
    result = pool->Acquire(&reused);
    allocated = failure.Triggered();
  }
  ASSERT_EQ(result, 0);
  EXPECT_FALSE(allocated);
  ASSERT_EQ(reused, block);
  out = static_cast<T*>(reused);
  verify(*out);
  for (size_t i = 0; i < fields.size(); ++i) {
    auto* str = out->*fields[i];
    ASSERT_EQ(str, strings[i]);
    EXPECT_EQ(str->data, buffers[i]);
    EXPECT_EQ(str->length, 0);
    EXPECT_EQ(str->data[0], '\0');
    // The whole configured buffer remains available after reuse.
    std::memset(str->data, 'y', 17);
    str->data[17] = '\0';
  }
  pool->ReturnBlock(reused);
}

}  // namespace

TEST_F(OperatorOutputPoolTest,
       MultiStringOutputsReuseAllFieldsWithoutAllocation) {
  CheckMultiStringOutputReuse<CompanyOperatorDocOutput>(
      "doc_out",
      {&CompanyOperatorDocOutput::intent_name,
       &CompanyOperatorDocOutput::answer_text},
      [](auto& out) {
        out.request_id = 99;
        out.status_code = -42;
        out.confidence = 0.9f;
        out.chunk_count = 3;
      },
      [](const auto& out) {
        EXPECT_EQ(out.request_id, 0u);
        EXPECT_EQ(out.status_code, 0);
        EXPECT_FLOAT_EQ(out.confidence, 0.0f);
        EXPECT_EQ(out.chunk_count, 0);
      });
  CheckMultiStringOutputReuse<CompanyOperatorAuditOutput>(
      "audit_out",
      {&CompanyOperatorAuditOutput::risk_level,
       &CompanyOperatorAuditOutput::matched_policy_clause,
       &CompanyOperatorAuditOutput::audit_verdict_json},
      [](auto& out) {
        out.request_id = 99;
        out.status_code = -42;
        out.risk_score = 0.9f;
      },
      [](const auto& out) {
        EXPECT_EQ(out.request_id, 0u);
        EXPECT_EQ(out.status_code, 0);
        EXPECT_FLOAT_EQ(out.risk_score, 0.0f);
      });
  CheckMultiStringOutputReuse<CompanyOperatorAudioOutput>(
      "audio_out",
      {&CompanyOperatorAudioOutput::transcribed_text,
       &CompanyOperatorAudioOutput::intent_slot_json},
      [](auto& out) {
        out.request_id = 99;
        out.status_code = -42;
      },
      [](const auto& out) {
        EXPECT_EQ(out.request_id, 0u);
        EXPECT_EQ(out.status_code, 0);
      });
}

TEST_F(OperatorOutputPoolTest, OdOutputReusePreservesOptionalMetadataStorage) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("od_out");
  ASSERT_NE(binding, nullptr);
  for (int32_t type_id : {0, 1, 4}) {
    SCOPED_TRACE(type_id);
    ResolvedOutputPoolSpec spec;
    spec.type = "od_out";
    spec.metadata_type_id = type_id;
    spec.meta_num = type_id == 0 ? 0 : 3;
    std::shared_ptr<OutputPoolState> pool;
    std::string err;
    ASSERT_EQ(OutputPoolState::Create(spec.type, 1, spec, binding, &pool, &err),
              0)
        << err;
    void* block = nullptr;
    ASSERT_EQ(pool->Acquire(&block), 0);
    auto* out = static_cast<CompanyOdOutput*>(block);
    EXPECT_EQ(out->request_id, 0u);
    EXPECT_EQ(out->detected_box_count, 0);
    EXPECT_EQ(out->status_code, 0);
    auto* metadata = out->metadata;
    void* payload = nullptr;
    if (type_id == 0) {
      EXPECT_EQ(metadata, nullptr);
    } else {
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->type_id, type_id);
      EXPECT_EQ(metadata->element_count, 0);
      EXPECT_EQ(metadata->byte_length, 0);
      payload = metadata->data;
      ASSERT_NE(payload, nullptr);
      metadata->element_count = spec.meta_num;
      metadata->byte_length =
          spec.meta_num * FindCompanyAnyType(type_id)->element_size;
      std::memset(payload, 1, metadata->byte_length);
    }
    auto* str = out->result_json;
    ASSERT_NE(str, nullptr);
    auto* data = str->data;
    ASSERT_NE(data, nullptr);
    std::strcpy(data, "{}");
    str->length = 2;
    out->request_id = 99;
    out->detected_box_count = 3;
    out->status_code = -42;
    void* reused = nullptr;
    int result = -1;
    bool allocated = false;
    {
      test_support::ScopedAllocationFailure failure(0);
      pool->ReturnBlock(block);
      result = pool->Acquire(&reused);
      allocated = failure.Triggered();
    }
    ASSERT_EQ(result, 0);
    EXPECT_FALSE(allocated);
    ASSERT_EQ(reused, block);
    EXPECT_EQ(out->request_id, 0u);
    EXPECT_EQ(out->detected_box_count, 0);
    EXPECT_EQ(out->status_code, 0);
    ASSERT_EQ(out->result_json, str);
    EXPECT_EQ(str->data, data);
    EXPECT_EQ(str->length, 0);
    EXPECT_EQ(data[0], '\0');
    ASSERT_EQ(out->metadata, metadata);
    if (metadata) {
      EXPECT_EQ(metadata->type_id, type_id);
      EXPECT_EQ(metadata->data, payload);
      EXPECT_EQ(metadata->element_count, 0);
      EXPECT_EQ(metadata->byte_length, 0);
    }
    pool->ReturnBlock(reused);
  }
}

TEST_F(OperatorOutputPoolTest, RerankOutputInitialAndReusedIndicesAreMinusOne) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("rerank_out");
  ASSERT_NE(binding, nullptr);
  ResolvedOutputPoolSpec spec;
  spec.type = "rerank_out";
  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(OutputPoolState::Create(spec.type, 1, spec, binding, &pool, &err),
            0)
      << err;
  void* previous = nullptr;
  for (int round = 0; round < 2; ++round) {
    void* block = nullptr;
    ASSERT_EQ(pool->Acquire(&block), 0);
    if (previous) {
      EXPECT_EQ(block, previous);
    }
    auto* out = static_cast<CompanyOperatorRerankOutput*>(block);
    EXPECT_EQ(out->request_id, 0u);
    EXPECT_EQ(out->count, 0);
    EXPECT_EQ(out->status_code, 0);
    out->request_id = 99;
    out->count = COMPANY_OPERATOR_MAX_RERANK_CANDIDATES;
    out->status_code = -42;
    for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
      EXPECT_FLOAT_EQ(out->scores[i], 0.0f);
      EXPECT_EQ(out->sorted_indices[i], -1);
      out->scores[i] = 0.9f;
      out->sorted_indices[i] = i;
    }
    pool->ReturnBlock(block);
    previous = block;
  }
}

// 4. ScopedOutputLeaseGuard 的 Untrack 与 Rollback 事务边界
TEST_F(OperatorOutputPoolTest, LeaseGuardTransactionRollback) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 3, spec, binding, &pool, &err), 0);
  EXPECT_EQ(pool->FreeBlockCount(), 3u);

  {
    ScopedOutputLeaseGuard guard;
    void *b1 = nullptr, *b2 = nullptr, *b3 = nullptr;
    ASSERT_EQ(pool->Acquire(&b1), 0);
    ASSERT_EQ(pool->Acquire(&b2), 0);
    ASSERT_EQ(pool->Acquire(&b3), 0);
    EXPECT_EQ(pool->FreeBlockCount(), 0u);

    guard.Track(pool, b1);
    guard.Track(pool, b2);
    guard.Track(pool, b3);

    // 模拟构造 shared_ptr 时将 b1 移交给外部智能指针 (Untrack)
    guard.Untrack(b1);

    // guard 析构触发 Rollback，b2 和 b3 应自动归还，b1 仍由模拟外部持有
  }

  EXPECT_EQ(pool->FreeBlockCount(), 2u);
  EXPECT_EQ(pool->CheckedOutCount(), 1u);
}

// 6. 多线程并发归还与条件变量唤醒
TEST_F(OperatorOutputPoolTest, ConcurrentAcquireReturnAndWakeup) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(binding, nullptr);

  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";

  std::shared_ptr<OutputPoolState> pool;
  std::string err;
  ASSERT_EQ(
      OutputPoolState::Create("keyword_out", 1, spec, binding, &pool, &err), 0);

  void* block = nullptr;
  ASSERT_EQ(pool->Acquire(&block), 0);
  EXPECT_EQ(pool->FreeBlockCount(), 0u);

  std::atomic<bool> worker_done{false};
  std::thread worker([&]() {
    void* acquired = nullptr;
    // 阻塞等待唤醒
    if (pool->Acquire(&acquired) == 0 && acquired != nullptr) {
      pool->ReturnBlock(acquired);
      worker_done = true;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_FALSE(worker_done);

  // 归还主线程块，触发唤醒
  pool->ReturnBlock(block);

  worker.join();
  EXPECT_TRUE(worker_done);
  EXPECT_EQ(pool->FreeBlockCount(), 1u);
  EXPECT_EQ(pool->CheckedOutCount(), 0u);
}

// 9. 严格 spec.type 与 64 MiB 预算边界拦截 (R9-005)
TEST_F(OperatorOutputPoolTest, StrictSpecTypeAndMemoryBudgetBoundary) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("doc_out");
  ASSERT_NE(binding, nullptr);

  // 1. spec.type 为空 -> 严格拒绝
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "";
    std::shared_ptr<OutputPoolState> pool;
    std::string err;
    int ret = OutputPoolState::Create("doc_out", 5, spec, binding, &pool, &err);
    EXPECT_EQ(ret, -2);
    EXPECT_EQ(pool, nullptr);
    EXPECT_FALSE(err.empty());
  }

  // 2. spec.type 类型不匹配 -> 严格拒绝
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "od_out";
    std::shared_ptr<OutputPoolState> pool;
    std::string err;
    int ret = OutputPoolState::Create("doc_out", 5, spec, binding, &pool, &err);
    EXPECT_EQ(ret, -2);
    EXPECT_EQ(pool, nullptr);
    EXPECT_FALSE(err.empty());
  }

  // 3. 超出 64 MiB 预算硬上限 -> 拒绝
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "doc_out";
    spec.capacities["intent_name"] = 255;
    spec.capacities["answer_text"] = 65536;
    std::shared_ptr<OutputPoolState> pool;
    std::string err;
    int ret = OutputPoolState::Create("doc_out", kMaxOutputPoolDepth, spec,
                                      binding, &pool, &err);
    EXPECT_EQ(ret, -2);
    EXPECT_EQ(pool, nullptr);
    EXPECT_NE(err.find("64 MiB"), std::string::npos);
  }
}

TEST_F(OperatorOutputPoolTest,
       EveryAllocationFailureReleasesResourcesAndAllowsRetry) {
  const char* types[] = {"keyword_out", "entity_out", "doc_out", "audit_out",
                         "audio_out",   "rerank_out", "od_out"};
  for (const char* type : types) {
    const std::string suffix(type);
    const auto* binding =
        OperatorValueTypeRegistry::Instance().GetBindingBySuffix(suffix);
    ASSERT_NE(binding, nullptr);
    ResolvedOutputPoolSpec spec;
    spec.type = suffix;
    if (suffix == "od_out") {
      spec.meta_num = 5;
      spec.metadata_type_id = 1;
    }
    // Multiple blocks exercise rollback after earlier blocks have succeeded.
    for (uint32_t depth : {1u, 3u}) {
      bool completed = false;
      for (int step = 0; step < 4096; ++step) {
        SCOPED_TRACE(suffix + " depth=" + std::to_string(depth) +
                     " allocation=" + std::to_string(step));
        bool injected = false;
        bool published = false;
        bool ready = false;
        bool overflowed = false;
        size_t outstanding = 0;
        int result = -4;
        {
          test_support::ScopedAllocationFailure failure(step);
          {
            std::shared_ptr<OutputPoolState> pool;
            std::string error;
            try {
              result = OutputPoolState::Create(suffix, depth, spec, binding,
                                               &pool, &error);
            } catch (const std::bad_alloc&) {
              // Preflight allocations may propagate to the Operator exception
              // barrier; they must also leave no pool or leaked allocations.
            }
            failure.DisableFailure();
            published = pool != nullptr;
            ready = pool && pool->Depth() == depth &&
                    pool->FreeBlockCount() == depth &&
                    pool->CheckedOutCount() == 0;
          }
          injected = failure.Triggered();
          outstanding = failure.Outstanding();
          overflowed = failure.Overflowed();
        }
        ASSERT_FALSE(overflowed);
        EXPECT_EQ(outstanding, 0u);
        if (!injected) {
          EXPECT_EQ(result, 0);
          EXPECT_TRUE(ready);
          completed = true;
          break;
        }
        EXPECT_NE(result, 0);
        EXPECT_FALSE(published);
        std::shared_ptr<OutputPoolState> recovered;
        std::string error;
        ASSERT_EQ(OutputPoolState::Create(suffix, depth, spec, binding,
                                          &recovered, &error),
                  0)
            << error;
        EXPECT_EQ(recovered->FreeBlockCount(), depth);
        EXPECT_EQ(recovered->CheckedOutCount(), 0u);
      }
      EXPECT_TRUE(completed)
          << "Allocation sweep never reached successful creation";
    }
  }
}

TEST_F(OperatorOutputPoolTest,
       PublicationAllocationFailuresRollbackWholeBatch) {
  constexpr uint32_t kDepth = 3;
  ResolvedOutputPoolSpec spec;
  spec.type = "keyword_out";
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(spec.type);
  std::shared_ptr<OutputPoolState> pool;
  std::string error;
  ASSERT_EQ(
      OutputPoolState::Create(spec.type, kDepth, spec, binding, &pool, &error),
      0);
  bool completed = false;
  for (int step = 0; step < 4096; ++step) {
    SCOPED_TRACE(step);
    operator_api::NamedIoBatch outputs(kDepth);
    std::vector<AcquiredOutputBlock> acquired;
    ScopedOutputLeaseGuard lease;
    lease.Reserve(kDepth);
    ASSERT_EQ(pool->FreeBlockCount(), kDepth);
    for (uint32_t i = 0; i < kDepth; ++i) {
      outputs[i]["client.keyword_out"] = nullptr;
      void* block = nullptr;
      ASSERT_EQ(pool->Acquire(&block), 0);
      lease.Track(pool, block);
      acquired.push_back({i, "client.keyword_out", pool, block});
    }
    bool threw = false;
    bool injected = false;
    bool all_null = false;
    bool all_present = false;
    size_t outstanding = 0;
    bool overflowed = false;
    {
      test_support::ScopedAllocationFailure failure(step);
      try {
        PublishOperatorOutputs(acquired, &outputs, &lease);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      failure.DisableFailure();
      all_null = true;
      all_present = true;
      for (const auto& frame : outputs) {
        all_null &= !frame.begin()->second;
        all_present &= static_cast<bool>(frame.begin()->second);
      }
      // The same guard used by Process rolls back any unpublished leases.
      lease.Rollback();
      outputs.clear();
      injected = failure.Triggered();
      outstanding = failure.Outstanding();
      overflowed = failure.Overflowed();
    }
    ASSERT_FALSE(overflowed);
    EXPECT_EQ(outstanding, 0u);
    EXPECT_EQ(pool->CheckedOutCount(), 0u);
    ASSERT_EQ(pool->FreeBlockCount(), kDepth);
    if (!injected) {
      EXPECT_FALSE(threw);
      EXPECT_TRUE(all_present);
      completed = true;
      break;
    }
    EXPECT_TRUE(threw);
    EXPECT_TRUE(all_null);
  }
  EXPECT_TRUE(completed)
      << "Allocation sweep never reached successful publication";
}

TEST_F(OperatorOutputPoolTest,
       AllocationFailureScopeRestoresAndIsolatesThreads) {
  std::atomic<bool> worker_start{false};
  bool worker_ok = false;
  std::thread worker([&] {
    while (!worker_start.load()) std::this_thread::yield();
    try {
      void* ptr = ::operator new(32);
      ::operator delete(ptr);
      worker_ok = true;
    } catch (...) {
    }
  });
  bool inner_threw = false;
  bool outer_threw = false;
  bool inner_injected = false;
  bool outer_injected = false;
  {
    test_support::ScopedAllocationFailure outer(0);
    worker_start = true;
    worker.join();
    {
      test_support::ScopedAllocationFailure inner(0);
      try {
        void* ptr = ::operator new(16);
        ::operator delete(ptr);
      } catch (const std::bad_alloc&) {
        inner_threw = true;
      }
      inner_injected = inner.Triggered();
    }
    try {
      void* ptr = ::operator new(16);
      ::operator delete(ptr);
    } catch (const std::bad_alloc&) {
      outer_threw = true;
    }
    outer_injected = outer.Triggered();
  }
  EXPECT_TRUE(worker_ok);
  EXPECT_TRUE(inner_threw && inner_injected);
  EXPECT_TRUE(outer_threw && outer_injected);
  // Unwinding an armed scope restores the default allocation behavior too.
  try {
    test_support::ScopedAllocationFailure failure(0);
    throw 1;
  } catch (int) {
  }
  EXPECT_NO_THROW({
    void* ptr = ::operator new(16);
    ::operator delete(ptr);
  });
}

TEST_F(OperatorOutputPoolTest,
       AllocationLedgerTracksScalarArrayAndAlignedStorage) {
  size_t live = 0;
  size_t released = 1;
  bool overflowed = false;
  bool nothrow_failed = false;
  bool aligned = false;
  {
    test_support::ScopedAllocationFailure scope;
    void* scalar = ::operator new(17);
    void* array = ::operator new[](33, std::nothrow);
    void* storage = ::operator new(65, std::align_val_t{64});
    void* aligned_array =
        ::operator new[](129, std::align_val_t{64}, std::nothrow);
    aligned = reinterpret_cast<uintptr_t>(storage) % 64 == 0 &&
              reinterpret_cast<uintptr_t>(aligned_array) % 64 == 0;
    live = scope.Outstanding();
    {
      test_support::ScopedAllocationFailure nested(0);
      void* failed = ::operator new(17, std::nothrow);
      nothrow_failed = failed == nullptr && nested.Triggered();
      ::operator delete(failed);
      // Deallocation in a nested scope must also update its parent's ledger.
      ::operator delete(scalar, size_t{17});
      ::operator delete[](array);
      ::operator delete(storage, size_t{65}, std::align_val_t{64});
      ::operator delete[](aligned_array, std::align_val_t{64});
    }
    released = scope.Outstanding();
    overflowed = scope.Overflowed();
  }
  EXPECT_EQ(live, 4u);
  EXPECT_EQ(released, 0u);
  EXPECT_FALSE(overflowed);
  EXPECT_TRUE(nothrow_failed);
  EXPECT_TRUE(aligned);
}

}  // namespace llm_edgeflow
