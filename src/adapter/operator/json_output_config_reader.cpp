#include "adapter/operator/json_output_config_reader.h"

#include "contracts/diagnostic.h"

namespace llm_edgeflow {

bool JsonOutputConfigReader::Read(OutputConfigField field, std::string* text,
                                  std::string* error) const noexcept {
  try {
    if (!text) {
      SetDiagnosticNoexcept(error,
                            "Null output configuration text destination");
      return false;
    }
    text->clear();
    if (!config_.is_object()) {
      SetDiagnosticNoexcept(error, "Output configuration must be an object");
      return false;
    }
    const char* key = nullptr;
    const char* fallback = nullptr;
    switch (field) {
      case OutputConfigField::kType:
        key = "type";
        break;
      case OutputConfigField::kAllocator:
        key = "allocator";
        fallback = "\"\"";
        break;
      case OutputConfigField::kParameters:
        key = "params";
        fallback = "{}";
        break;
      case OutputConfigField::kCapacities:
        key = "capacities";
        fallback = "{}";
        break;
      case OutputConfigField::kMetadataCount:
        key = "meta_num";
        fallback = "0";
        break;
      case OutputConfigField::kMetadataTypeId:
        key = "metadata_type_id";
        fallback = "0";
        break;
      default:
        SetDiagnosticNoexcept(error, "Unknown output configuration field");
        return false;
    }
    const auto value = config_.find(key);
    if (value == config_.end()) {
      if (!fallback) {
        SetDiagnosticNoexcept(error,
                              "Missing required output configuration type");
        return false;
      }
      *text = fallback;
    } else {
      if (field == OutputConfigField::kType &&
          (!value->is_string() ||
           value->get_ref<const std::string&>().empty())) {
        SetDiagnosticNoexcept(
            error, "Output configuration type must be a nonempty string");
        return false;
      }
      *text = value->dump();
    }
    return true;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(error, e.what());
  } catch (...) {
    SetDiagnosticNoexcept(error,
                          "Unknown exception reading output configuration");
  }
  if (text) text->clear();
  return false;
}

}  // namespace llm_edgeflow
