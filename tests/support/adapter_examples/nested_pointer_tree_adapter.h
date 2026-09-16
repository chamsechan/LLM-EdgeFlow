#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {
namespace template_examples {

struct TemplateTreeNode {
  int node_id;
  const char* node_name;
  int child_count;
  const struct TemplateTreeNode** children;  // 嵌套指针树 (多级指针)
};

typedef struct {
  uint64_t request_id;
  const struct TemplateTreeNode* root_node;  // 根节点
} TemplateNestedTreeInput;

typedef struct {
  uint64_t request_id;
  int status_code;
  int total_nodes_traversed;
  char traversal_path[512];
} TemplateNestedTreeOutput;

struct TemplateTreeNodeDto {
  int node_id = 0;
  std::string node_name;
  std::vector<TemplateTreeNodeDto> children;
};

struct TemplateTreeResultDto {
  uint64_t request_id;
  int status_code;
  int total_nodes_traversed;
  std::string traversal_path;
};

class TemplateNestedPointerTreeAdapter {
 public:
  const char* AdapterName() const { return "TemplateNestedPointerTree"; }
  size_t MaxBatchSize() const { return 64; }

  static bool UnpackNodeRecursive(const TemplateTreeNode* node,
                                  TemplateTreeNodeDto* out_dto,
                                  int current_depth, int max_depth,
                                  int sample_idx, const char* biz_name,
                                  AdapterStatus* out_status) {
    if (!node) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput("Null tree node pointer",
                                                  "node", sample_idx, biz_name);
      }
      return false;
    }
    if (current_depth > max_depth) {
      if (out_status) {
        *out_status =
            AdapterStatus::InvalidInput("Tree depth exceeds maximum limit (" +
                                            std::to_string(max_depth) + ")",
                                        "node.depth", sample_idx, biz_name);
      }
      return false;
    }
    if (!AdapterValidationHelper::RequireBoundedString(
            "node.node_name", node->node_name, 4096, sample_idx, biz_name,
            out_status)) {
      return false;
    }
    if (!AdapterValidationHelper::RequireRange(
            "node.child_count", node->child_count, 0, 100, sample_idx, biz_name,
            out_status)) {
      return false;
    }

    out_dto->node_id = node->node_id;
    out_dto->node_name = node->node_name;

    if (node->child_count > 0) {
      if (!AdapterValidationHelper::RequireNotNull("node.children",
                                                   node->children, sample_idx,
                                                   biz_name, out_status)) {
        return false;
      }
      out_dto->children.resize(node->child_count);
      for (int i = 0; i < node->child_count; ++i) {
        if (!UnpackNodeRecursive(node->children[i], &out_dto->children[i],
                                 current_depth + 1, max_depth, sample_idx,
                                 biz_name, out_status)) {
          return false;
        }
      }
    }
    return true;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, static_cast<int>(MaxBatchSize()), AdapterName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed", "inputs",
          AdapterName());
    }

    std::vector<uint64_t> req_ids;
    std::vector<TemplateTreeNodeDto> root_dtos;
    req_ids.reserve(num_inputs);
    root_dtos.reserve(num_inputs);

    constexpr int kMaxTreeDepth = 32;

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const TemplateNestedTreeInput*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireNotNull("inputs[i].root_node",
                                                   in->root_node, i,
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      TemplateTreeNodeDto root_dto;
      if (!UnpackNodeRecursive(in->root_node, &root_dto, 1, kMaxTreeDepth, i,
                               AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      root_dtos.push_back(std::move(root_dto));
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, "raw_request_ids", std::move(req_ids), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, "tree_root_dtos", std::move(root_dtos), AdapterName(),
            out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* res =
        ctx->Read<std::vector<TemplateTreeResultDto>>("tree_final_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, AdapterName(), out_status);
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<TemplateNestedTreeOutput*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;
      out_ptr->total_nodes_traversed = (*res)[i].total_nodes_traversed;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->traversal_path, sizeof(out_ptr->traversal_path),
              (*res)[i].traversal_path.c_str(), "outputs[i].traversal_path", i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

}  // namespace template_examples
}  // namespace llm_edgeflow
