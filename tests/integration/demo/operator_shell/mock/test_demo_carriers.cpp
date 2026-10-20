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
  auto builder = DemoIoRegistry::Instance().FindInput("MockInput");
  ASSERT_NE(builder, nullptr);
  llm_edgeflow::operator_api::OperatorIoEntry input;
  input.type = "keyword_in";
  input.name = "keyword_match";
  DemoRequestBatch batch;
  ASSERT_EQ(builder(options, {input}, &batch), 0);
  ASSERT_EQ(batch.requests.size(), 3u);
  auto moved = std::move(batch);
  ASSERT_NE(moved.storage, nullptr);
  const auto text = [&](size_t row) {
    const auto* root = static_cast<const MockInput*>(
        moved.requests[row].at("demo.keyword_in").get());
    EXPECT_EQ(root->byte_size, static_cast<int32_t>(sizeof(MockInput)));
    EXPECT_EQ(root->type, kMockInputMultidata);
    EXPECT_EQ(root->num_frame, 1);
    EXPECT_EQ(root->data_pool->byte_size,
              static_cast<int32_t>(sizeof(MockDataPool)));
    EXPECT_EQ(root->data_pool->data_type, kMockDataText);
    const auto* leaf =
        static_cast<const MockTextInput*>(root->data_pool->data_ptr);
    EXPECT_EQ(leaf->byte_size, static_cast<int32_t>(sizeof(MockTextInput)));
    return std::string(leaf->value, leaf->length);
  };
  EXPECT_EQ(text(0), "初始化完成");
  EXPECT_EQ(text(1), "ordinary text");
}

TEST_F(DemoCarrierTest, JsonDisplayCopiesExactSdkDocumentBeforeCarrierReuse) {
  auto show = DemoIoRegistry::Instance().FindOutput("MockOutput");
  ASSERT_NE(show, nullptr);
  std::string bytes =
      R"({"unknown":[false,0,{"unicode":"中文"}],"text":"  keep\n  "})";
  MockTextOutput text{sizeof(MockTextOutput), bytes.data(),
                      static_cast<int32_t>(bytes.size())};
  MockOutput carrier{sizeof(MockOutput), kMockOutputOc, 0, &text};
  Json copied;
  show(&carrier, Json::object(), &copied);
  EXPECT_EQ(copied, Json::parse(bytes));
  bytes.assign(bytes.size(), 'x');
  EXPECT_EQ(copied["unknown"][2]["unicode"], "中文");
}

}  // namespace
}  // namespace alg_demo
