#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "adapter/business_adapter_registry.h"
#include "adapter/platform/platform_business_bridge_registry.h"

namespace alg_framework {
namespace {

TEST(PlatformBusinessBridgeRegistryTest, AllSevenBusinessesSelfRegistered) {
  const auto& reg = PlatformBusinessBridgeRegistry::Instance();
  for (int biz_id = 1; biz_id <= 7; ++biz_id) {
    auto biz_type = static_cast<CompanyAlgBizType>(biz_id);
    const auto* desc = reg.GetBridge(biz_type);
    ASSERT_NE(desc, nullptr) << "Business " << biz_id << " must be registered";
    EXPECT_EQ(desc->biz_type, biz_type);
    EXPECT_FALSE(desc->biz_name.empty());
    EXPECT_FALSE(desc->internal_input_type_name.empty());
    EXPECT_FALSE(desc->internal_output_type_name.empty());
    EXPECT_FALSE(desc->registration_identity.empty());
    EXPECT_FALSE(desc->input_slots.empty());
    EXPECT_FALSE(desc->output_slots.empty());
    EXPECT_NE(desc->convert_sample_input, nullptr);
    EXPECT_NE(desc->convert_sample_output, nullptr);
    EXPECT_NE(desc->create_shadow_output_dto, nullptr);
  }
}

TEST(PlatformBusinessBridgeRegistryTest, GlobalInitIsIdempotentAndThreadSafe) {
  auto& reg = PlatformBusinessBridgeRegistry::Instance();
  EXPECT_EQ(reg.GlobalInit(), 0);
  EXPECT_EQ(reg.GlobalInit(), 0);
  EXPECT_FALSE(reg.HasConflict());

  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&]() {
      for (int k = 0; k < 100; ++k) {
        for (int biz_id = 1; biz_id <= 7; ++biz_id) {
          const auto* desc =
              reg.GetBridge(static_cast<CompanyAlgBizType>(biz_id));
          EXPECT_NE(desc, nullptr);
        }
      }
    });
  }
  for (auto& t : workers) {
    t.join();
  }
}

TEST(PlatformBusinessBridgeRegistryTest,
     IsolatedRegistryIdempotencyAndLateRegistration) {
  PlatformBusinessBridgeRegistry local_reg;

  // Copy bridges from global instance to local registry
  const auto& global_reg = PlatformBusinessBridgeRegistry::Instance();
  for (int biz_id = 1; biz_id <= 7; ++biz_id) {
    const auto* desc =
        global_reg.GetBridge(static_cast<CompanyAlgBizType>(biz_id));
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(local_reg.RegisterBridge(*desc));
  }

  // First GlobalInit succeeds
  EXPECT_EQ(local_reg.GlobalInit(), 0);

  // Second GlobalInit is idempotent and succeeds
  EXPECT_EQ(local_reg.GlobalInit(), 0);
  EXPECT_FALSE(local_reg.HasConflict());

  // Late registration after audited_ returns false but does NOT pollute
  // conflict state
  PlatformBusinessBridgeDescriptor dummy_desc;
  dummy_desc.biz_type = static_cast<CompanyAlgBizType>(1);
  dummy_desc.biz_name = "DocQA";
  EXPECT_FALSE(local_reg.RegisterBridge(dummy_desc));

  // GlobalInit continues to succeed idempotently
  EXPECT_EQ(local_reg.GlobalInit(), 0);
  EXPECT_FALSE(local_reg.HasConflict());
}

TEST(PlatformBusinessBridgeRegistryTest,
     IsolatedRegistryRejectsConflictingDescriptor) {
  PlatformBusinessBridgeRegistry local_reg;

  const auto* orig_desc =
      PlatformBusinessBridgeRegistry::Instance().GetBridge(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(orig_desc, nullptr);
  EXPECT_TRUE(local_reg.RegisterBridge(*orig_desc));

  // Re-register identical descriptor with same identity -> succeeds
  EXPECT_TRUE(local_reg.RegisterBridge(*orig_desc));

  // Re-register with different identity or slots -> conflict
  PlatformBusinessBridgeDescriptor conflict_desc = *orig_desc;
  conflict_desc.registration_identity = "ConflictingIdentityDocQA";
  EXPECT_FALSE(local_reg.RegisterBridge(conflict_desc));
  EXPECT_TRUE(local_reg.HasConflict());
  EXPECT_EQ(local_reg.GlobalInit(), -6);
}

TEST(PlatformBusinessBridgeRegistryTest,
     IsolatedRegistryRejectsSlotDirectionMismatch) {
  PlatformBusinessBridgeRegistry local_reg;

  const auto* orig_desc =
      PlatformBusinessBridgeRegistry::Instance().GetBridge(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(orig_desc, nullptr);

  PlatformBusinessBridgeDescriptor bad_desc = *orig_desc;
  bad_desc.biz_type = static_cast<CompanyAlgBizType>(99);
  bad_desc.input_slots[0].direction = IoDirection::kOutput;  // Mismatch
  EXPECT_FALSE(local_reg.RegisterBridge(bad_desc));
  EXPECT_TRUE(local_reg.HasConflict());
  EXPECT_EQ(local_reg.GlobalInit(), -6);
}

}  // namespace
}  // namespace alg_framework
