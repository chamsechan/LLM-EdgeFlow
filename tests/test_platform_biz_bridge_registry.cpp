#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "adapter/biz_adapter_registry.h"
#include "adapter/platform/platform_biz_bridge_registry.h"

namespace alg_framework {
namespace {

TEST(PlatformBizBridgeRegistryTest, AllSevenBusinessesSelfRegistered) {
  const auto& reg = PlatformBizBridgeRegistry::Instance();
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

TEST(PlatformBizBridgeRegistryTest, GlobalInitIsIdempotentAndThreadSafe) {
  auto& reg = PlatformBizBridgeRegistry::Instance();
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

TEST(PlatformBizBridgeRegistryTest,
     IsolatedRegistryIdempotencyAndLateRegistration) {
  PlatformBizBridgeRegistry local_reg;

  // Copy bridges from global instance to local registry
  const auto& global_reg = PlatformBizBridgeRegistry::Instance();
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
  PlatformBizBridgeDescriptor dummy_desc;
  dummy_desc.biz_type = static_cast<CompanyAlgBizType>(1);
  dummy_desc.biz_name = "DocQA";
  EXPECT_FALSE(local_reg.RegisterBridge(dummy_desc));

  // GlobalInit continues to succeed idempotently
  EXPECT_EQ(local_reg.GlobalInit(), 0);
  EXPECT_FALSE(local_reg.HasConflict());
}

TEST(PlatformBizBridgeRegistryTest,
     IsolatedRegistryRejectsConflictingDescriptor) {
  PlatformBizBridgeRegistry local_reg;

  const auto* orig_desc =
      PlatformBizBridgeRegistry::Instance().GetBridge(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(orig_desc, nullptr);
  EXPECT_TRUE(local_reg.RegisterBridge(*orig_desc));

  // Re-register identical descriptor with same identity -> succeeds
  EXPECT_TRUE(local_reg.RegisterBridge(*orig_desc));

  // Re-register with different identity or slots -> conflict
  PlatformBizBridgeDescriptor conflict_desc = *orig_desc;
  conflict_desc.registration_identity = "ConflictingIdentityDocQA";
  EXPECT_FALSE(local_reg.RegisterBridge(conflict_desc));
  EXPECT_TRUE(local_reg.HasConflict());
  EXPECT_EQ(local_reg.GlobalInit(), -6);
}

TEST(PlatformBizBridgeRegistryTest,
     IsolatedRegistryRejectsEachIndividualCallbackChange) {
  PlatformBizBridgeRegistry local_reg;

  const auto* orig_desc =
      PlatformBizBridgeRegistry::Instance().GetBridge(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(orig_desc, nullptr);
  EXPECT_TRUE(local_reg.RegisterBridge(*orig_desc));

  // 1. Only convert_sample_input changed -> conflict
  {
    PlatformBizBridgeRegistry r;
    EXPECT_TRUE(r.RegisterBridge(*orig_desc));
    PlatformBizBridgeDescriptor conflict = *orig_desc;
    conflict.convert_sample_input =
        [](const std::unordered_map<std::string, const void*>&,
           ProcessLocalShadowStorage&, const void**,
           std::string*) -> int { return -99; };
    EXPECT_FALSE(r.RegisterBridge(conflict));
    EXPECT_TRUE(r.HasConflict());
    EXPECT_EQ(r.GlobalInit(), -6);
  }

  // 2. Only convert_sample_output changed -> conflict
  {
    PlatformBizBridgeRegistry r;
    EXPECT_TRUE(r.RegisterBridge(*orig_desc));
    PlatformBizBridgeDescriptor conflict = *orig_desc;
    conflict.convert_sample_output = [](const void*, void*,
                                        const ResolvedOutputPoolSpec&,
                                        std::string*) -> int { return -99; };
    EXPECT_FALSE(r.RegisterBridge(conflict));
    EXPECT_TRUE(r.HasConflict());
    EXPECT_EQ(r.GlobalInit(), -6);
  }

  // 3. Only create_shadow_output_dto changed -> conflict
  {
    PlatformBizBridgeRegistry r;
    EXPECT_TRUE(r.RegisterBridge(*orig_desc));
    PlatformBizBridgeDescriptor conflict = *orig_desc;
    conflict.create_shadow_output_dto =
        [](ProcessLocalShadowStorage&) -> void* { return nullptr; };
    EXPECT_FALSE(r.RegisterBridge(conflict));
    EXPECT_TRUE(r.HasConflict());
    EXPECT_EQ(r.GlobalInit(), -6);
  }
}

TEST(PlatformBizBridgeRegistryTest,
     SevenBusinesses64SampleDirectDtoConversionMatrix) {
  const auto& reg = PlatformBizBridgeRegistry::Instance();
  constexpr size_t kNumSamples = 64;

  // 1. KeywordMatch 64 samples
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_KEYWORD_MATCH);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "keyword_out";

    std::vector<std::string> raw_strings(kNumSamples);
    std::vector<CompanyString> c_strings(kNumSamples);
    std::vector<CompanyPlatformKeywordInput> inputs(kNumSamples);

    for (size_t i = 0; i < kNumSamples; ++i) {
      // 前 15 帧穷尽 1～15 字节，后续交替覆盖长字符串。
      if (i < 15) {
        raw_strings[i] = std::string(i + 1, 'k');
      } else if (i % 2 == 0) {
        raw_strings[i] = "kw_" + std::to_string(i);
      } else {
        raw_strings[i] = "Long customer complaint and urgent keyword string #" +
                         std::to_string(i);
      }
      c_strings[i] = CompanyString{static_cast<int32_t>(raw_strings[i].size()),
                                   raw_strings[i].data()};
      inputs[i].request_id = 1000 + i;
      inputs[i].sentence_text = &c_strings[i];

      std::unordered_map<std::string, const void*> slots = {
          {"keyword_in", &inputs[i]}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      ASSERT_NE(internal_dto, nullptr);

      const auto* dto =
          static_cast<const CompanyKeywordInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 1000 + i);
      EXPECT_STREQ(dto->sentence_text, raw_strings[i].c_str());

      // Output mirror conversion test
      CompanyKeywordOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      out_dto.is_hit = (i % 2 == 0) ? 1 : 0;
      std::snprintf(out_dto.match_result_json,
                    sizeof(out_dto.match_result_json), "{\"match_idx\":%zu}",
                    i);

      char out_buf[256] = {0};
      CompanyString out_cs{0, out_buf};
      CompanyPlatformKeywordOutput out_struct{};
      out_struct.match_result_json = &out_cs;

      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 1000 + i);
      EXPECT_EQ(out_struct.is_hit, out_dto.is_hit);
      EXPECT_STREQ(out_struct.match_result_json->data,
                   out_dto.match_result_json);
      EXPECT_EQ(out_struct.match_result_json->length,
                static_cast<int32_t>(std::strlen(out_dto.match_result_json)));
    }
  }

  // 2. EntityExtract 64 samples
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_ENTITY_EXTRACT);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "entity_out";

    for (size_t i = 0; i < kNumSamples; ++i) {
      std::string raw_str = (i < 10) ? ("E" + std::to_string(i))
                                     : ("Entity extraction sentence #" +
                                        std::to_string(i) + " in Beijing");
      CompanyString cs{static_cast<int32_t>(raw_str.size()), raw_str.data()};
      CompanyPlatformEntityInput input{2000 + i, &cs};

      std::unordered_map<std::string, const void*> slots = {
          {"entity_in", &input}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      const auto* dto =
          static_cast<const CompanyEntityInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 2000 + i);
      EXPECT_STREQ(dto->sentence_text, raw_str.c_str());

      CompanyEntityOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      std::snprintf(out_dto.entities_json, sizeof(out_dto.entities_json),
                    "[\"Entity_%zu\"]", i);

      char out_buf[256] = {0};
      CompanyString out_cs{0, out_buf};
      CompanyPlatformEntityOutput out_struct{};
      out_struct.entities_json = &out_cs;

      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 2000 + i);
      EXPECT_STREQ(out_struct.entities_json->data, out_dto.entities_json);
    }
  }

  // 3. DocQA 64 samples
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_DOC_QA);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "doc_out";

    for (size_t i = 0; i < kNumSamples; ++i) {
      std::string q_str = "Question #" + std::to_string(i);
      std::string d_str =
          "Context document text for sample #" + std::to_string(i);
      CompanyString q_cs{static_cast<int32_t>(q_str.size()), q_str.data()};
      CompanyString d_cs{static_cast<int32_t>(d_str.size()), d_str.data()};
      CompanyPlatformDocInput input{3000 + i, &d_cs, &q_cs};

      std::unordered_map<std::string, const void*> slots = {{"doc_in", &input}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      const auto* dto = static_cast<const CompanyDocInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 3000 + i);
      EXPECT_STREQ(dto->query_text, q_str.c_str());
      EXPECT_STREQ(dto->doc_text, d_str.c_str());

      CompanyDocOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      out_dto.confidence = 0.95f;
      out_dto.chunk_count = 4;
      std::snprintf(out_dto.intent_name, sizeof(out_dto.intent_name),
                    "INTENT_%zu", i);
      std::snprintf(out_dto.answer_text, sizeof(out_dto.answer_text),
                    "Answer_%zu", i);

      char intent_buf[64] = {0};
      char answer_buf[1024] = {0};
      CompanyString intent_cs{0, intent_buf};
      CompanyString answer_cs{0, answer_buf};
      CompanyPlatformDocOutput out_struct{};
      out_struct.intent_name = &intent_cs;
      out_struct.answer_text = &answer_cs;

      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 3000 + i);
      EXPECT_FLOAT_EQ(out_struct.confidence, 0.95f);
      EXPECT_EQ(out_struct.chunk_count, 4);
      EXPECT_STREQ(out_struct.intent_name->data, out_dto.intent_name);
      EXPECT_STREQ(out_struct.answer_text->data, out_dto.answer_text);
    }
  }

  // 4. ComplianceAudit 64 samples
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_COMPLIANCE_AUDIT);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "audit_out";

    for (size_t i = 0; i < kNumSamples; ++i) {
      std::string u_str = "Audit user prompt text #" + std::to_string(i);
      std::string c_str = "channel_" + std::to_string(i % 5);
      CompanyString u_cs{static_cast<int32_t>(u_str.size()), u_str.data()};
      CompanyString c_cs{static_cast<int32_t>(c_str.size()), c_str.data()};
      CompanyPlatformAuditInput input{4000 + i, &u_cs, &c_cs};

      std::unordered_map<std::string, const void*> slots = {
          {"audit_in", &input}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      const auto* dto =
          static_cast<const CompanyAuditInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 4000 + i);
      EXPECT_STREQ(dto->user_text, u_str.c_str());
      EXPECT_STREQ(dto->channel_name, c_str.c_str());

      CompanyAuditOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      out_dto.risk_score = 0.1f * (i % 10);
      std::snprintf(out_dto.risk_level, sizeof(out_dto.risk_level), "SAFE");
      std::snprintf(out_dto.matched_policy_clause,
                    sizeof(out_dto.matched_policy_clause), "Policy Clause #%zu",
                    i);
      std::snprintf(out_dto.audit_verdict_json,
                    sizeof(out_dto.audit_verdict_json), "{\"verdict\":%zu}", i);

      char r_buf[32] = {0}, p_buf[256] = {0}, v_buf[1024] = {0};
      CompanyString r_cs{0, r_buf}, p_cs{0, p_buf}, v_cs{0, v_buf};
      CompanyPlatformAuditOutput out_struct{};
      out_struct.risk_level = &r_cs;
      out_struct.matched_policy_clause = &p_cs;
      out_struct.audit_verdict_json = &v_cs;

      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 4000 + i);
      EXPECT_STREQ(out_struct.risk_level->data, "SAFE");
      EXPECT_STREQ(out_struct.matched_policy_clause->data,
                   out_dto.matched_policy_clause);
      EXPECT_STREQ(out_struct.audit_verdict_json->data,
                   out_dto.audit_verdict_json);
    }
  }

  // 5. AudioAsrIntent 64 samples
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_AUDIO_ASR_INTENT);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "audio_out";

    std::vector<std::vector<float>> pcm_pool(kNumSamples,
                                             std::vector<float>(16000));
    for (size_t i = 0; i < kNumSamples; ++i) {
      pcm_pool[i][0] = 0.001f * static_cast<float>(i);
      pcm_pool[i][15999] = 0.002f * static_cast<float>(i) + 0.5f;
      CompanyPlatformAudioInput input{5000 + i, pcm_pool[i].data(), 16000,
                                      16000};

      std::unordered_map<std::string, const void*> slots = {
          {"audio_in", &input}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      const auto* dto =
          static_cast<const CompanyAudioInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 5000 + i);
      EXPECT_EQ(dto->pcm_length, 16000);
      EXPECT_EQ(dto->sample_rate, 16000);
      EXPECT_FLOAT_EQ(dto->pcm_buffer[0], pcm_pool[i][0]);
      EXPECT_FLOAT_EQ(dto->pcm_buffer[15999], pcm_pool[i][15999]);

      CompanyAudioOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      std::snprintf(out_dto.transcribed_text, sizeof(out_dto.transcribed_text),
                    "Transcribed speech audio #%zu", i);
      std::snprintf(out_dto.intent_slot_json, sizeof(out_dto.intent_slot_json),
                    "{\"intent\":\"AUDIO_%zu\"}", i);

      char t_buf[512] = {0}, slot_buf[1024] = {0};
      CompanyString t_cs{0, t_buf}, slot_cs{0, slot_buf};
      CompanyPlatformAudioOutput out_struct{};
      out_struct.transcribed_text = &t_cs;
      out_struct.intent_slot_json = &slot_cs;

      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 5000 + i);
      EXPECT_STREQ(out_struct.transcribed_text->data, out_dto.transcribed_text);
      EXPECT_STREQ(out_struct.intent_slot_json->data, out_dto.intent_slot_json);
    }
  }

  // 6. CrossRerank 64 samples with 8 distinct candidates
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_CROSS_RERANK);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "rerank_out";

    for (size_t i = 0; i < kNumSamples; ++i) {
      std::string q_str = "Rerank Query #" + std::to_string(i);
      CompanyString q_cs{static_cast<int32_t>(q_str.size()), q_str.data()};
      std::vector<std::string> c_strs(8);
      std::vector<CompanyString> c_cs(8);
      CompanyPlatformRerankInput input{};
      input.request_id = 6000 + i;
      input.query_text = &q_cs;
      input.candidate_count = 8;

      for (int c = 0; c < 8; ++c) {
        c_strs[c] =
            "Passage " + std::to_string(c) + " for req " + std::to_string(i);
        c_cs[c] = CompanyString{static_cast<int32_t>(c_strs[c].size()),
                                c_strs[c].data()};
        input.candidate_passages[c] = &c_cs[c];
      }

      std::unordered_map<std::string, const void*> slots = {
          {"rerank_in", &input}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      const auto* dto =
          static_cast<const CompanyRerankBatchInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 6000 + i);
      EXPECT_STREQ(dto->query_text, q_str.c_str());
      EXPECT_EQ(dto->candidate_count, 8);
      for (int c = 0; c < 8; ++c) {
        EXPECT_STREQ(dto->candidate_passages[c], c_strs[c].c_str());
      }

      CompanyRerankBatchOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      out_dto.count = 8;
      for (int c = 0; c < 8; ++c) {
        out_dto.scores[c] = 0.1f * static_cast<float>(8 - c);
        out_dto.sorted_indices[c] = c;
      }

      CompanyPlatformRerankOutput out_struct{};
      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 6000 + i);
      EXPECT_EQ(out_struct.count, 8);
      for (int c = 0; c < 8; ++c) {
        EXPECT_FLOAT_EQ(out_struct.scores[c], out_dto.scores[c]);
        EXPECT_EQ(out_struct.sorted_indices[c], out_dto.sorted_indices[c]);
      }
    }
  }

  // 7. OcrDocQA 64 samples
  {
    const auto* desc = reg.GetBridge(ALG_BIZ_TYPE_OCR_DOC_QA);
    ASSERT_NE(desc, nullptr);
    ProcessLocalShadowStorage storage;
    ResolvedOutputPoolSpec spec;
    spec.type = "od_out";

    for (size_t i = 0; i < kNumSamples; ++i) {
      std::string uri_str = "data/invoices/inv_" + std::to_string(i) + ".jpg";
      std::string q_str =
          "Extract total amount for invoice #" + std::to_string(i);
      CompanyString uri_cs{static_cast<int32_t>(uri_str.size()),
                           uri_str.data()};
      CompanyString q_cs{static_cast<int32_t>(q_str.size()), q_str.data()};
      CompanyFrame frame{7000 + i, &uri_cs, nullptr};

      std::unordered_map<std::string, const void*> slots = {{"frame", &frame},
                                                            {"string", &q_cs}};
      const void* internal_dto = nullptr;
      std::string err;
      ASSERT_EQ(desc->convert_sample_input(slots, storage, &internal_dto, &err),
                0);
      const auto* dto =
          static_cast<const CompanyOcrDocInputStruct*>(internal_dto);
      EXPECT_EQ(dto->request_id, 7000 + i);
      EXPECT_STREQ(dto->image_path, uri_str.c_str());
      EXPECT_STREQ(dto->query_prompt, q_str.c_str());

      CompanyOcrDocOutputStruct out_dto{};
      out_dto.request_id = dto->request_id;
      out_dto.detected_box_count = static_cast<int>(i % 12);
      std::snprintf(out_dto.extracted_invoice_json,
                    sizeof(out_dto.extracted_invoice_json), "{\"inv_id\":%zu}",
                    i);

      char res_buf[2048] = {0};
      CompanyString res_cs{0, res_buf};
      CompanyOdOutput out_struct{};
      out_struct.result_json = &res_cs;

      ASSERT_EQ(desc->convert_sample_output(&out_dto, &out_struct, spec, &err),
                0);
      EXPECT_EQ(out_struct.request_id, 7000 + i);
      EXPECT_EQ(out_struct.detected_box_count, out_dto.detected_box_count);
      EXPECT_STREQ(out_struct.result_json->data,
                   out_dto.extracted_invoice_json);
    }
  }
}

TEST(PlatformBizBridgeRegistryTest,
     IsolatedRegistryRejectsSlotDirectionMismatch) {
  PlatformBizBridgeRegistry local_reg;

  const auto* orig_desc =
      PlatformBizBridgeRegistry::Instance().GetBridge(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(orig_desc, nullptr);

  PlatformBizBridgeDescriptor bad_desc = *orig_desc;
  bad_desc.biz_type = static_cast<CompanyAlgBizType>(99);
  bad_desc.input_slots[0].direction = IoDirection::kOutput;  // Mismatch
  EXPECT_FALSE(local_reg.RegisterBridge(bad_desc));
  EXPECT_TRUE(local_reg.HasConflict());
  EXPECT_EQ(local_reg.GlobalInit(), -6);
}

TEST(PlatformBizBridgeRegistryTest, ConcurrentReadFreezeInterleavingTSan) {
  PlatformBizBridgeRegistry local_reg;
  const auto& global_reg = PlatformBizBridgeRegistry::Instance();
  for (int biz_id = 1; biz_id <= 7; ++biz_id) {
    const auto* desc =
        global_reg.GetBridge(static_cast<CompanyAlgBizType>(biz_id));
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(local_reg.RegisterBridge(*desc));
  }

  std::atomic<bool> stop_flag{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&]() {
      while (!stop_flag.load()) {
        for (int biz_id = 1; biz_id <= 7; ++biz_id) {
          const auto* desc =
              local_reg.GetBridge(static_cast<CompanyAlgBizType>(biz_id));
          EXPECT_NE(desc, nullptr);
        }
      }
    });
  }

  std::thread freezer([&]() {
    for (int i = 0; i < 50; ++i) {
      EXPECT_EQ(local_reg.GlobalInit(), 0);
      PlatformBizBridgeDescriptor late_desc;
      late_desc.biz_type = static_cast<CompanyAlgBizType>(99);
      EXPECT_FALSE(local_reg.RegisterBridge(late_desc));
    }
  });

  freezer.join();
  stop_flag.store(true);
  for (auto& r : readers) {
    r.join();
  }
  EXPECT_EQ(local_reg.GlobalInit(), 0);
  EXPECT_FALSE(local_reg.HasConflict());
}

}  // namespace
}  // namespace alg_framework
