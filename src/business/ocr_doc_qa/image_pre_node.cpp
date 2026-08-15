#include <iostream>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 图片与发票前处理算子
 */
class ImagePreNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* raw_images =
        req_ctx->Get<std::vector<std::string>>("raw_image_paths");
    auto* raw_queries = req_ctx->Get<std::vector<std::string>>("raw_queries");
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");

    if (!raw_images || !raw_queries || !req_ids) {
      req_ctx->SetError(-5101,
                        "ImagePreNode: Missing input image or query tensors");
      return -5101;
    }

    std::vector<TraceableItem<std::string>> image_items;
    for (size_t i = 0; i < raw_images->size(); ++i) {
      image_items.emplace_back((*req_ids)[i], 0, (*raw_images)[i]);
    }

    req_ctx->Set("traceable_image_items", std::move(image_items));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "ImagePreNode";
    return name;
  }
};

REGISTER_NODE(ImagePreNode);

}  // namespace alg_framework
