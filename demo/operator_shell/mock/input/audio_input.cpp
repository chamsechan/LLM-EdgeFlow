#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

int BuildAudioRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 1) return 3;

  struct Storage {
    std::vector<AudioDatasetSample> dataset_samples;
    std::vector<std::vector<float>> fallback_buffers;
    std::vector<CompanyOperatorAudioInput> carriers;
  };
  auto storage = std::make_shared<Storage>();
  const bool is_real_profile = options.suite == "real";
  bool loaded_from_dataset = false;
  if (!options.dataset_path.empty()) {
    const std::string resolved = ResolvePath(options.dataset_path);
    if (!std::filesystem::exists(resolved)) {
      if (!options.allow_fallback_sample) {
        std::cerr << "[AudioAsrIntentDemo ERROR] Dataset file not found: "
                  << options.dataset_path << std::endl;
        return 4;
      }
    } else if (resolved.rfind(".jsonl") != std::string::npos) {
      std::string read_err;
      if (!ReadAudioDataset(options.dataset_path, &storage->dataset_samples,
                            &read_err)) {
        if (!options.allow_fallback_sample) {
          std::cerr
              << "[AudioAsrIntentDemo ERROR] Failed to read audio dataset: "
              << read_err << std::endl;
          return 4;
        }
      } else {
        loaded_from_dataset = true;
      }
    } else {
      if (is_real_profile && !options.allow_fallback_sample) {
        std::cerr
            << "[AudioAsrIntentDemo ERROR] Real audio ASR profile requires a "
               ".jsonl dataset, got: "
            << options.dataset_path << std::endl;
        return 4;
      }
    }
  }

  if (is_real_profile && !loaded_from_dataset &&
      !options.allow_fallback_sample) {
    std::cerr
        << "[AudioAsrIntentDemo ERROR] Real audio ASR profile requires a valid "
           "audio dataset and fallback sample is disabled."
        << std::endl;
    return 4;
  }

  if (loaded_from_dataset) {
    storage->carriers.reserve(storage->dataset_samples.size());
    for (const auto& sample : storage->dataset_samples) {
      storage->carriers.push_back(
          {inputs[0].service_type.value_or(0), sample.pcm_data.data(),
           static_cast<int32_t>(sample.pcm_data.size()), sample.sample_rate});
    }
  } else {
    if (is_real_profile && !options.allow_fallback_sample) {
      std::cerr
          << "[AudioAsrIntentDemo ERROR] Real audio ASR profile cannot use "
             "fallback fixed audio."
          << std::endl;
      return 4;
    }
    storage->fallback_buffers.emplace_back(16000, 0.01f);
    storage->carriers.push_back(
        {inputs[0].service_type.value_or(0),
         storage->fallback_buffers[0].data(),
         static_cast<int32_t>(storage->fallback_buffers[0].size()), 16000});
  }

  DemoRequestBatch batch;
  batch.requests.resize(storage->carriers.size());
  for (size_t i = 0; i < storage->carriers.size(); ++i) {
    batch.requests[i]["demo." + inputs[0].type] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->carriers[i]);
  }
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorAudioInput", BuildAudioRequests);

}  // namespace
}  // namespace alg_demo
