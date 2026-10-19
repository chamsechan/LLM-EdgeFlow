#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "demo/common/demo_io_registry.h"
#include "nlohmann/json.hpp"

// mock 载体的 Demo 构造与展示；运行器与结果写入由通用 Demo 测试覆盖。
namespace alg_demo {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

class DemoCarrierTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (fs::temp_directory_path() / "demo-carrier-XXXXXX").string();
    char* created = mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory = created;
    dataset = directory / "input.txt";
    std::ofstream(dataset, std::ios::binary)
        << "# ignored comment\n  初始化完成  \n\nordinary text\n自检完成\n";
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }
  fs::path directory, dataset;
};

TEST_F(DemoCarrierTest, CarrierStorageRetainsTrimmedUtf8AcrossBatchMoves) {
  DemoOptions options;
  options.dataset_path = dataset.string();
  auto builder =
      DemoIoRegistry::Instance().FindInput("CompanyOperatorKeywordInput");
  ASSERT_NE(builder, nullptr);
  llm_edgeflow::operator_api::OperatorIoEntry input;
  input.type = "keyword_in";
  input.service_type = 123;
  DemoRequestBatch batch;
  ASSERT_EQ(builder(options, {input}, &batch), 0);
  ASSERT_EQ(batch.requests.size(), 3u);
  auto moved = std::move(batch);
  ASSERT_NE(moved.storage, nullptr);
  const auto* first = static_cast<const CompanyOperatorKeywordInput*>(
      moved.requests[0].at("demo.keyword_in").get());
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->service_type, 123);
  EXPECT_EQ(
      std::string(first->sentence_text->data, first->sentence_text->length),
      "初始化完成");
  const auto* second = static_cast<const CompanyOperatorKeywordInput*>(
      moved.requests[1].at("demo.keyword_in").get());
  EXPECT_EQ(
      std::string(second->sentence_text->data, second->sentence_text->length),
      "ordinary text");
}

TEST_F(DemoCarrierTest, JsonDisplayCopiesExactSdkDocumentBeforeCarrierReuse) {
  auto show = DemoIoRegistry::Instance().FindOutput("CompanyOperatorOutput");
  ASSERT_NE(show, nullptr);
  std::string bytes =
      R"({"unknown":[false,0,{"unicode":"中文"}],"text":"  keep\n  "})";
  CompanyString text{static_cast<int32_t>(bytes.size()), bytes.data()};
  CompanyOperatorOutput carrier{0, &text, -17};
  Json copied;
  int32_t status = 0;
  show(&carrier, Json::object(), &status, &copied);
  EXPECT_EQ(status, -17);
  EXPECT_EQ(copied, Json::parse(bytes));
  bytes.assign(bytes.size(), 'x');
  EXPECT_EQ(copied["unknown"][2]["unicode"], "中文");
}

}  // namespace
}  // namespace alg_demo
