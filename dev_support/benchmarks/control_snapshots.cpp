#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"
// Counts ordinary C++ new/new[] on the calling thread only. Aligned
// allocations, direct malloc calls, and allocations on other threads are not
// included.
thread_local bool count_on = false;
thread_local size_t alloc_count = 0, alloc_bytes = 0;
void* operator new(size_t n) {
  if (count_on) {
    ++alloc_count;
    alloc_bytes += n;
  }
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
using namespace llm_edgeflow;
int main(int argc, char** argv) {
  if (argc != 3 ||
      (std::string(argv[1]) != "template" && std::string(argv[1]) != "rules") ||
      (std::string(argv[2]) != "0" && std::string(argv[2]) != "1")) {
    std::cerr << "Usage: " << argv[0] << " template|rules 0|1\n";
    return 1;
  }
  bool tpl = std::string(argv[1]) == "template",
       concurrent = std::atoi(argv[2]);
  const char* type = tpl ? "TextTemplateNode" : "TextRuleMatchNode";
  auto node = NodeRegistry::Instance().Create(type);
  SessionContext session;
  nlohmann::json cfg =
      tpl ? nlohmann::json{{"template", "V0: {{primary}} / {{role}}"},
                           {"values", {{"role", "assistant"}}}}
          : nlohmann::json{{"categories", {{"GREETING", {"hello", "hi"}}}},
                           {"rules", nlohmann::json::array(
                                         {{{"id", "world"},
                                           {"strategy", "regex"},
                                           {"pattern", "hello (?<tail>world)"},
                                           {"category", "WORLD"}}})}};
  if (!node || !InitNodeForTest(*node, cfg, &session)) return 2;
  int cmd = tpl ? kControlCmdUpdatePrompt : kControlCmdUpdateRules;
  std::string update =
      tpl ? R"({"template":"V1: {{primary}} / {{role}}","prompt_id":"pid_1"})"
          : R"({"categories":{"GREETING":["hello","hi"],"EXTRA":["absent"]}})";
  node->Control(cmd, update);  // warm schema statics
  for (int a = 0; a < 5; ++a) {
    alloc_count = alloc_bytes = 0;
    count_on = true;
    auto r = node->Control(cmd, update);
    count_on = false;
    if (r.status != NodeControlStatus::kHandled) return 3;
    std::cout << "ALLOC " << alloc_count << " " << alloc_bytes << "\n";
  }
  TextBatch input;
  for (int i = 0; i < 50; ++i)
    input.emplace_back(100, i, "hello world sample " + std::to_string(i));
  for (int w = 0; w < 100; ++w) {
    AlgContext ctx;
    ctx.Publish(tpl ? "primary" : "text", input);
    if (node->Process(&ctx)) return 4;
  }
  constexpr int n = 2000;
  std::vector<std::unique_ptr<AlgContext>> contexts;
  for (int i = 0; i < n; ++i) {
    auto ctx = std::make_unique<AlgContext>();
    ctx->Publish(tpl ? "primary" : "text", input);
    contexts.push_back(std::move(ctx));
  }
  std::atomic<bool> stop{false}, ready{false};
  std::atomic<size_t> updates{0};
  std::thread writer;
  if (concurrent) {
    writer = std::thread([&] {
      ready = true;
      while (!stop) {
        if (node->Control(cmd, update).status != NodeControlStatus::kHandled)
          std::abort();
        ++updates;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
    while (!ready) std::this_thread::yield();
  }
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i)
    if (node->Process(contexts[i].get())) std::abort();
  auto end = std::chrono::steady_clock::now();
  stop = true;
  if (concurrent) writer.join();
  for (auto& context : contexts) {
    auto& ctx = *context;
    if (tpl) {
      auto* out = ctx.Read<TextBatch>("text");
      if (!out || out->size() != 50 ||
          (*out)[0].data != "V1: hello world sample 0 / assistant")
        return 5;
    } else {
      auto* out = ctx.Read<RuleMatchBatch>("matches");
      if (!out || out->size() != 50 || (*out)[0].data.category != "GREETING" ||
          (*out)[0].data.captures.at("tail") != "world")
        return 6;
    }
  }
  std::cout << "RESULT " << argv[1] << " " << concurrent << " "
            << std::chrono::duration<double, std::micro>(end - start).count() /
                   n
            << " " << updates << "\n";
}
