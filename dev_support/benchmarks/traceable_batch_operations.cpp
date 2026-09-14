// Standalone Linux measurement; build/run commands and limits are recorded in
// doc/rfcs/reviews/0055-traceable-batch-verification.md.
#include "nodes/traceable_batch_operations.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace llm_edgeflow;

template <typename Fn>
double Measure(Fn fn, size_t expected_size) {
  std::vector<double> times;
  for (int round = 0; round < 8; ++round) {
    const auto start = std::chrono::steady_clock::now();
    auto result = fn();
    const auto end = std::chrono::steady_clock::now();
    if (!result.ok() || result.value().size() != expected_size) {
      throw std::runtime_error("Unexpected benchmark output");
    }
    if (round > 0) {  // One warmup, then seven measured constructions.
      times.push_back(
          std::chrono::duration<double, std::milli>(end - start).count());
    }
  }
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0]
              << " join|group|select|scatter|split item_count\n";
    return 1;
  }
  const std::string operation = argv[1];
  const size_t count = std::stoull(argv[2]);
  if (count == 0 || count > 1000000 || count % 4 != 0) return 1;
  TextBatch anchor;
  anchor.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    anchor.emplace_back(static_cast<uint32_t>(i / 4),
                        static_cast<uint32_t>(i % 4),
                        std::string(128, i % 2 == 0 ? 'a' : 'b'));
  }
  auto other = anchor;
  std::mt19937 rng(55);
  std::shuffle(other.begin(), other.end(), rng);
  auto predicate = [](const std::string& s) { return s.front() == 'a'; };
  double median_ms = 0;
  if (operation == "join") {
    median_ms = Measure([&] { return JoinByItem(anchor, other); }, count);
  } else if (operation == "group") {
    median_ms =
        Measure([&] { return GroupByRequest(anchor, other); }, count / 4);
  } else if (operation == "select") {
    median_ms =
        Measure([&] { return SelectBatch(anchor, predicate); }, count / 2);
  } else if (operation == "scatter") {
    auto selection = SelectBatch(anchor, predicate);
    if (!selection.ok()) return 2;
    auto replacements = selection.value().Materialize();
    std::shuffle(replacements.begin(), replacements.end(), rng);
    median_ms = Measure(
        [&] { return ScatterReplace(selection.value(), replacements); }, count);
  } else if (operation == "split") {
    median_ms = Measure(
        [&]() -> NodeResult<TextBatch> {
          auto result = SplitPayloads(anchor, [](const std::string& s) {
            return std::vector<std::string>{s.substr(0, 64), s.substr(64)};
          });
          if (!result.ok()) {
            return NodeResult<TextBatch>::Failure(
                std::move(result).ExtractFailure());
          }
          return NodeResult<TextBatch>::Success(
              std::move(result.value().children));
        },
        count * 2);
  } else {
    return 1;
  }
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 2;
  std::cout << nlohmann::json({{"operation", operation},
                               {"items", count},
                               {"median_ms", median_ms},
                               {"process_peak_rss_kib", usage.ru_maxrss}})
                   .dump()
            << '\n';
}
