#include <gtest/gtest.h>

#include "core/session_context.h"
#include "nodes/session_resources.h"

namespace llm_edgeflow {
namespace {
class RegisteredIdentity final : public IModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string name = "identity_impl";
    return name;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string type = "identity_type";
    return type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
};

ModelRegistration Registration(std::string name,
                               std::shared_ptr<IModel> model) {
  ModelRegistration value;
  value.model_name = std::move(name);
  value.model = std::move(model);
  value.impl_name = "identity_impl";
  value.model_type = "identity_type";
  value.backend_type = "neutral";
  value.revision = "r1";
  return value;
}

TEST(ModelManagerOwnershipTest, InvalidBatchesDoNotCommitTheirValidPrefix) {
  ModelManager manager;
  auto model = std::make_shared<RegisteredIdentity>();
  ASSERT_TRUE(manager.RegisterBatch({Registration("existing", model)}));
  struct Case {
    const char* name;
    ModelRegistration bad;
  };
  auto wrong_impl = Registration("new", model);
  wrong_impl.impl_name = "different";
  auto wrong_type = Registration("new", model);
  wrong_type.model_type = "different";
  auto missing_revision = Registration("new", model);
  missing_revision.revision.clear();
  const Case cases[] = {
      {"existing name", Registration("existing", model)},
      {"duplicate staged name", Registration("prefix", model)},
      {"missing model", Registration("new", nullptr)},
      {"missing instance name", Registration("", model)},
      {"implementation identity", wrong_impl},
      {"category identity", wrong_type},
      {"incomplete revision metadata", missing_revision}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    EXPECT_FALSE(
        manager.RegisterBatch({Registration("prefix", model), item.bad}));
    EXPECT_FALSE(manager.HasModel("prefix"));
    EXPECT_EQ(manager.GetModel<IModel>("existing"), model);
    EXPECT_EQ(manager.GetAllModels().size(), 1U);
  }
  ASSERT_TRUE(manager.RegisterBatch(
      {Registration("prefix", model), Registration("new", model)}));
  EXPECT_EQ(manager.GetModel<RegisteredIdentity>("new"), model);
  EXPECT_EQ(manager.GetModel<ILlmModel>("new"), nullptr);
}

TEST(ModelManagerOwnershipTest, RevisionAndMetadataAreOwnedSessionSnapshots) {
  SessionContext session;
  auto model = std::make_shared<RegisteredIdentity>();
  std::weak_ptr<IModel> lifetime = model;
  auto registration = Registration("model", model);
  registration.revision.clear();
  registration.model_file = "weights.bin";
  registration.model_params = {{"size", 2}};
  registration.backend_params = {{"device", 0}};
  ASSERT_TRUE(session.GetModelManager().RegisterBatch({registration}));
  auto before = session.GetModelManager().GetModelRegistration("model");
  ASSERT_TRUE(before);
  EXPECT_EQ(
      before->revision,
      "identity_impl\nneutral\nweights.bin\n{\"size\":2}\n{\"device\":0}");
  registration.model_params["size"] = 99;
  EXPECT_EQ(before->model_params, (nlohmann::json{{"size", 2}}));
  EXPECT_EQ(
      session.GetModelManager().GetModelRegistration("model")->model_params,
      (nlohmann::json{{"size", 2}}));
  SessionResources resources(session);
  ASSERT_TRUE(session.GetModelManager().UpdateModelRevision("model", "r2"));
  EXPECT_EQ(resources.GetModelRevision("model"), "r2");
  EXPECT_NE(before->revision, "r2");
  EXPECT_FALSE(session.GetModelManager().UpdateModelRevision("model", ""));
  EXPECT_FALSE(session.GetModelManager().UpdateModelRevision("missing", "r3"));
  before.reset();
  registration.model.reset();
  model.reset();
  EXPECT_FALSE(lifetime.expired());
}
}  // namespace
}  // namespace llm_edgeflow
