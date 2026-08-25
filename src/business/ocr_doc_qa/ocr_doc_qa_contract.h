#pragma once

#include <string>
#include <vector>

#include "business/ocr_doc_qa/ocr_doc_qa_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/traceable_item.h"

namespace alg_framework {

inline constexpr BlackboardKey<std::vector<std::string>> kRawImagePaths{
    "raw_image_paths", "vector<string>"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kTraceableImageItems{"traceable_image_items", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<int>> kOcrBoxCounts{"ocr_box_counts",
                                                               "vector<int>"};

inline constexpr BlackboardKey<std::vector<OcrDocResult>> kOcrDocFinalOutputs{
    "ocr_doc_final_outputs", "vector<OcrDocResult>"};

}  // namespace alg_framework
