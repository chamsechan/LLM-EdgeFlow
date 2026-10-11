#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "core/session_context.h"
#include "nodes/session_resources.h"

namespace llm_edgeflow {
struct SessionResourceTestAccess {
  static bool WaitForCallers(SessionContext& session, const std::string& key,
                             long callers) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      {
        std::unique_lock<std::mutex> lock(session.resource_mutex_,
                                          std::try_to_lock);
        if (lock.owns_lock()) {
          const auto entry = session.flights_.find(key);
          // One reference in the map, one in each participating invocation.
          if (entry != session.flights_.end() &&
              entry->second.use_count() == callers + 1)
            return true;
        }
      }
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
};
namespace {

TEST(SessionResourcesTest,
     OneFactoryServesAnExactCohortAndSessionOwnsTheValue) {
  std::weak_ptr<std::string> lifetime;
  {
    SessionContext session;
    const SessionResourceKey<std::string> key("dictionary");
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto resume = release.get_future().share();
    std::atomic<int> calls{0};
    auto factory = [&] {
      ++calls;
      entered.set_value();
      // A different resource may be constructed while this factory is active.
      auto other =
          session.GetOrCreateResource(SessionResourceKey<int>("revision"),
                                      [] { return std::make_shared<int>(9); });
      if (*other != 9) throw std::logic_error("other resource lost");
      resume.wait();
      return std::make_shared<std::string>("owned dictionary");
    };
    std::vector<std::future<std::shared_ptr<std::string>>> results;
    results.push_back(std::async(std::launch::async, [&] {
      return session.GetOrCreateResource(key, factory);
    }));
    started.wait();
    for (int i = 1; i < 8; ++i) {
      results.push_back(std::async(std::launch::async, [&] {
        return session.GetOrCreateResource(key, factory);
      }));
    }
    const bool joined =
        SessionResourceTestAccess::WaitForCallers(session, key.Name(), 8);
    std::atomic<int> wrong_factory_calls{0};
    auto wrong_type = std::async(std::launch::async, [&] {
      try {
        session.GetOrCreateResource(SessionResourceKey<int>(key.Name()), [&] {
          ++wrong_factory_calls;
          return std::make_shared<int>(1);
        });
        return false;
      } catch (const std::logic_error&) {
        return true;
      }
    });
    auto wrong_set = std::async(std::launch::async, [&] {
      try {
        session.SetResource(SessionResourceKey<int>(key.Name()),
                            std::make_shared<int>(1));
        return false;
      } catch (const std::logic_error&) {
        return true;
      }
    });
    const bool rejected_without_waiting =
        wrong_type.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    const bool set_rejected_without_waiting =
        wrong_set.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    release.set_value();
    std::shared_ptr<std::string> first;
    for (auto& result : results) {
      auto value = result.get();
      if (!first) first = value;
      EXPECT_EQ(value, first);
    }
    EXPECT_TRUE(wrong_type.get());
    EXPECT_TRUE(wrong_set.get());
    EXPECT_TRUE(rejected_without_waiting);
    EXPECT_TRUE(set_rejected_without_waiting);
    EXPECT_EQ(wrong_factory_calls, 0);
    ASSERT_TRUE(joined);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(*first, "owned dictionary");
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(session.GetOrCreateResource(
                  key,
                  [&] {
                    ++wrong_factory_calls;
                    return std::make_shared<std::string>("unexpected factory");
                  }),
              first);
    EXPECT_THROW(
        session.GetOrCreateResource(SessionResourceKey<int>(key.Name()),
                                    [&] {
                                      ++wrong_factory_calls;
                                      return std::make_shared<int>(2);
                                    }),
        std::logic_error);
    EXPECT_EQ(wrong_factory_calls, 0);
    lifetime = first;
    first.reset();
    EXPECT_FALSE(lifetime.expired());
    EXPECT_THROW(session.GetResource(SessionResourceKey<int>(key.Name())),
                 std::logic_error);
  }
  EXPECT_TRUE(lifetime.expired());
}

TEST(SessionResourcesTest, FailureIsSharedOnlyWithinItsFlightAndCanBeRetried) {
  SessionContext session;
  const SessionResourceKey<int> key("index");
  std::promise<void> entered, release;
  auto started = entered.get_future();
  auto resume = release.get_future().share();
  std::atomic<int> calls{0};
  // Keep the injected exception alive until every waiter has finished reading
  // it.
  const auto injected =
      std::make_exception_ptr(std::runtime_error("index unavailable"));
  auto factory = [&]() -> std::shared_ptr<int> {
    ++calls;
    entered.set_value();
    resume.wait();
    std::rethrow_exception(injected);
  };
  auto invoke = [&] {
    try {
      session.GetOrCreateResource(key, factory);
      return std::string("missing failure");
    } catch (const std::runtime_error& error) {
      return std::string(error.what());
    }
  };
  std::vector<std::future<std::string>> results;
  results.push_back(std::async(std::launch::async, invoke));
  started.wait();
  for (int i = 1; i < 8; ++i)
    results.push_back(std::async(std::launch::async, invoke));
  const bool joined =
      SessionResourceTestAccess::WaitForCallers(session, key.Name(), 8);
  release.set_value();
  for (auto& result : results) EXPECT_EQ(result.get(), "index unavailable");
  EXPECT_TRUE(joined);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(session.GetResource(key), nullptr);
  auto recovered = session.GetOrCreateResource(key, [&] {
    ++calls;
    return std::make_shared<int>(17);
  });
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(*recovered, 17);
  EXPECT_EQ(calls, 2);
}

TEST(SessionResourcesTest, NullAndTypedFacadeFailuresAreNotCached) {
  SessionContext session;
  const SessionResourceKey<int> key("nullable");
  EXPECT_EQ(
      session.GetOrCreateResource(key, [] { return std::shared_ptr<int>{}; }),
      nullptr);
  auto recovered =
      session.GetOrCreateResource(key, [] { return std::make_shared<int>(4); });
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(*recovered, 4);
  SessionResources resources(session);
  const SessionResourceKey<int> typed("typed");
  NodeFailure failure;
  failure.kind = NodeErrorKind::kInputError;
  failure.message = "typed failure";
  failure.cause_code = -77;
  failure.stage = "hydrate";
  failure.model_slot = "dictionary";
  failure.source_location = "resource producer";
  failure.batch_detail =
      BatchFailureDetail{"resource factory", BatchFailureReason::kMissing,
                         TraceableItemKey{15, 9}};
  auto failed = resources.GetOrCreateResult<int>(
      typed, [&] { return NodeResult<int>::Failure(failure); });
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.failure().kind, NodeErrorKind::kInputError);
  EXPECT_EQ(failed.failure().cause_code, -77);
  EXPECT_EQ(failed.failure().message, "typed failure");
  EXPECT_EQ(failed.failure().stage, "hydrate");
  EXPECT_EQ(failed.failure().model_slot, "dictionary");
  EXPECT_EQ(failed.failure().source_location, "resource producer");
  ASSERT_TRUE(failed.failure().batch_detail);
  EXPECT_EQ(failed.failure().batch_detail->operation, "resource factory");
  EXPECT_EQ(failed.failure().batch_detail->reason,
            BatchFailureReason::kMissing);
  EXPECT_EQ(failed.failure().batch_detail->key, (TraceableItemKey{15, 9}));
  auto retry = resources.GetOrCreateResult<int>(
      typed, [] { return NodeResult<int>::Success(8); });
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(*retry.value(), 8);
  const SessionResourceKey<int> throwing("unknown failure");
  EXPECT_THROW(resources.GetOrCreateResult<int>(
                   throwing, []() -> NodeResult<int> { throw 23; }),
               int);
  EXPECT_EQ(session.GetResource(throwing), nullptr);
  EXPECT_THROW(session.SetResource(SessionResourceKey<double>("typed"),
                                   std::make_shared<double>(1)),
               std::logic_error);
}
}  // namespace
}  // namespace llm_edgeflow
