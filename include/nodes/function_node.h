#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/config_schema_validation.h"
#include "contracts/traceable_item.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/port_definition.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "nodes/model_calls.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"
#include "nodes/parameter_binding.h"
#include "nodes/traceable_algorithms.h"
#include "nodes/traceable_batch_validation.h"

namespace llm_edgeflow {

template <typename T>
struct BatchItemTraits {
  static constexpr bool kIsTraceableBatch = false;
  using PayloadType = void;
  using ItemType = void;
  using BatchType = T;
};

template <typename PayloadT>
struct BatchItemTraits<std::vector<TraceableItem<PayloadT>>> {
  static constexpr bool kIsTraceableBatch = true;
  using PayloadType = PayloadT;
  using ItemType = TraceableItem<PayloadT>;
  using BatchType = std::vector<TraceableItem<PayloadT>>;
};

template <typename BatchT>
struct Input {
  static_assert(BatchItemTraits<BatchT>::kIsTraceableBatch,
                "Input batch type must be a vector of TraceableItem<T>");
  using BatchType = BatchT;
  using PayloadType = typename BatchItemTraits<BatchT>::PayloadType;

  std::string name;
  explicit Input(std::string port_name) : name(std::move(port_name)) {}
};

template <typename BatchT>
struct Output {
  static_assert(BatchItemTraits<BatchT>::kIsTraceableBatch,
                "Output batch type must be a vector of TraceableItem<T>");
  using BatchType = BatchT;
  using PayloadType = typename BatchItemTraits<BatchT>::PayloadType;

  std::string name;
  explicit Output(std::string port_name) : name(std::move(port_name)) {}
};

template <typename T>
struct IsNodeResult : std::false_type {
  using ValueType = T;
};

template <typename T>
struct IsNodeResult<NodeResult<T>> : std::true_type {
  using ValueType = T;
};

namespace detail {

template <typename Fn, typename InT, typename ParamsT>
auto InvokeMapItem(const Fn& fn, const InT& item, const ParamsT& params) {
  if constexpr (std::is_invocable_v<Fn, const InT&, const ParamsT&>) {
    return fn(item, params);
  } else if constexpr (std::is_invocable_v<Fn, const InT&>) {
    return fn(item);
  } else {
    static_assert(
        std::is_invocable_v<Fn, const InT&, const ParamsT&> ||
            std::is_invocable_v<Fn, const InT&>,
        "Map function must accept either (const InPayload&, const Params&) "
        "or (const InPayload&)");
  }
}

template <typename T>
struct MemberFunctionTraits;

template <typename Class, typename Ret, typename... Args>
struct MemberFunctionTraits<Ret (Class::*)(Args...) const> {
  using ClassType = Class;
  using ReturnType = Ret;
};

template <typename Class, typename Ret, typename... Args>
struct MemberFunctionTraits<Ret (Class::*)(Args...)> {
  using ClassType = Class;
  using ReturnType = Ret;
};

template <typename Fn, typename InputsT, typename ParamsT, typename ModelsT>
auto InvokeBatch(const Fn& fn, const InputsT& inputs, const ParamsT& params,
                 const ModelsT& models) {
  if constexpr (std::is_member_function_pointer_v<Fn>) {
    using ClassT = typename MemberFunctionTraits<Fn>::ClassType;
    ClassT logic{};
    return (logic.*fn)(inputs, params, models);
  } else {
    return fn(inputs, params, models);
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Map Entrypoint
// ---------------------------------------------------------------------------

template <typename InputBatchT, typename OutputBatchT, typename ParamsT,
          typename MapFnT>
class MapSpec {
 public:
  using InputBatch = InputBatchT;
  using OutputBatch = OutputBatchT;
  using ParametersType = ParamsT;
  using InPayload = typename BatchItemTraits<InputBatchT>::PayloadType;
  using OutPayload = typename BatchItemTraits<OutputBatchT>::PayloadType;
  using RawResult = decltype(detail::InvokeMapItem(
      std::declval<MapFnT>(), std::declval<const InPayload&>(),
      std::declval<const ParamsT&>()));
  using UnwrappedResult = typename IsNodeResult<RawResult>::ValueType;
  static constexpr bool kReturnsNodeResult = IsNodeResult<RawResult>::value;

  static_assert(std::is_same_v<UnwrappedResult, OutPayload>,
                "Map function return payload type must match OutputBatch "
                "element payload type");

  MapSpec(Input<InputBatchT> in, Output<OutputBatchT> out,
          Parameters<ParamsT> params, MapFnT fn)
      : in_(std::move(in)),
        out_(std::move(out)),
        params_(std::move(params)),
        fn_(std::move(fn)) {}

  MapSpec& Description(std::string desc) & {
    description_ = std::move(desc);
    return *this;
  }
  MapSpec Description(std::string desc) && {
    description_ = std::move(desc);
    return std::move(*this);
  }

  MapSpec& Category(std::string cat) & {
    category_ = std::move(cat);
    return *this;
  }
  MapSpec Category(std::string cat) && {
    category_ = std::move(cat);
    return std::move(*this);
  }

  MapSpec& ParallelSafe(bool safe) & {
    parallel_safe_ = safe;
    return *this;
  }
  MapSpec ParallelSafe(bool safe) && {
    parallel_safe_ = safe;
    return std::move(*this);
  }

  MapSpec& BizNames(std::vector<std::string> biz_names) {
    biz_names_ = std::move(biz_names);
    return *this;
  }

  const std::string& InputName() const noexcept { return in_.name; }
  const std::string& OutputName() const noexcept { return out_.name; }
  const Parameters<ParamsT>& ParametersSpec() const noexcept { return params_; }
  const MapFnT& Function() const noexcept { return fn_; }

  NodeDefinition BuildDefinition(std::string node_type) const {
    NodeDefinition def;
    def.node_type = std::move(node_type);
    def.category = category_;
    def.description = description_;
    def.parallel_safe = parallel_safe_;
    def.biz_names = biz_names_;
    def.inputs = {NodePortDefinition{
        in_.name, BlackboardTypeTraits<InputBatchT>::TypeName(), true, "1:1",
        "preserve", "request"}};
    def.outputs = {NodePortDefinition{
        out_.name, BlackboardTypeTraits<OutputBatchT>::TypeName(), true, "1:1",
        "preserve", "request"}};
    def.config_fields = params_.Fields();
    def.validate_config = [params = params_](
                              const nlohmann::json& cfg,
                              const std::unordered_set<std::string>& conn,
                              std::string* err) {
      return params.ValidateWithBindings(cfg, conn, err);
    };
    return def;
  }

 private:
  Input<InputBatchT> in_;
  Output<OutputBatchT> out_;
  Parameters<ParamsT> params_;
  MapFnT fn_;
  std::string category_ = "custom";
  std::string description_;
  bool parallel_safe_ = false;
  std::vector<std::string> biz_names_;
};

template <typename InputBatchT, typename OutputBatchT, typename ParamsT,
          typename MapFnT>
inline auto MakeMapSpec(Input<InputBatchT> in, Output<OutputBatchT> out,
                        Parameters<ParamsT> params, MapFnT fn) {
  return MapSpec<InputBatchT, OutputBatchT, ParamsT, MapFnT>(
      std::move(in), std::move(out), std::move(params), std::move(fn));
}

template <typename InputBatchT, typename OutputBatchT, typename MapFnT>
inline auto MakeMapSpec(Input<InputBatchT> in, Output<OutputBatchT> out,
                        NoParameters, MapFnT fn) {
  return MapSpec<InputBatchT, OutputBatchT, NoParameters, MapFnT>(
      std::move(in), std::move(out), Parameters<NoParameters>{}, std::move(fn));
}

template <typename InputBatchT, typename OutputBatchT, typename MapFnT>
inline auto MakeMapSpec(Input<InputBatchT> in, Output<OutputBatchT> out,
                        MapFnT fn) {
  return MapSpec<InputBatchT, OutputBatchT, NoParameters, MapFnT>(
      std::move(in), std::move(out), Parameters<NoParameters>{}, std::move(fn));
}

// ---------------------------------------------------------------------------
// Batch Entrypoint
// ---------------------------------------------------------------------------

enum class InputFlow {
  PreserveByRequest,
  AggregateByRequest,
};

class IProvenanceReader {
 public:
  virtual ~IProvenanceReader() = default;
  virtual size_t Size() const noexcept = 0;
  virtual uint64_t ReqId(size_t index) const noexcept = 0;
  virtual uint32_t SubId(size_t index) const noexcept = 0;
};

template <typename BatchT>
class TraceableBatchProvenanceReader : public IProvenanceReader {
 public:
  explicit TraceableBatchProvenanceReader(const BatchT& batch)
      : batch_(batch) {}
  size_t Size() const noexcept override { return batch_.size(); }
  uint64_t ReqId(size_t index) const noexcept override {
    return batch_[index].req_id;
  }
  uint32_t SubId(size_t index) const noexcept override {
    return batch_[index].sub_id;
  }

 private:
  const BatchT& batch_;
};

template <typename InputsT>
class InputPortBinding {
 public:
  virtual ~InputPortBinding() = default;
  virtual const std::string& LogicalName() const = 0;
  virtual NodePortDefinition ToPortDefinition() const = 0;
  virtual bool IsRequired() const = 0;
  virtual bool BindPort(const NodeInitContext& init_ctx) = 0;
  virtual bool PopulateInput(const AlgContext& ctx, InputsT* inputs,
                             std::string* err) const = 0;
  virtual const void* GetRawBatch(const InputsT& inputs) const = 0;
  virtual bool HasBatch(const InputsT& inputs) const = 0;
  virtual TraceableAlignmentResult ValidateAlignment(
      const InputsT& inputs, const IProvenanceReader& output_reader) const = 0;
  virtual std::unique_ptr<InputPortBinding<InputsT>> Clone() const = 0;
};

template <typename InputsT, typename BatchT>
class ConcreteInputPortBinding final : public InputPortBinding<InputsT> {
 public:
  using MemberPtr = const BatchT* InputsT::*;

  ConcreteInputPortBinding(std::string name, MemberPtr member_ptr,
                           bool required, InputFlow flow)
      : name_(std::move(name)),
        member_ptr_(member_ptr),
        required_(required),
        flow_(flow),
        in_port_(name_) {}

  const std::string& LogicalName() const override { return name_; }
  bool IsRequired() const override { return required_; }

  NodePortDefinition ToPortDefinition() const override {
    std::string card = (flow_ == InputFlow::AggregateByRequest ? "N:1" : "1:1");
    std::string prov =
        (flow_ == InputFlow::AggregateByRequest ? "aggregate" : "preserve");
    return NodePortDefinition{
        name_,           BlackboardTypeTraits<BatchT>::TypeName(),
        required_,       std::move(card),
        std::move(prov), "request"};
  }

  bool BindPort(const NodeInitContext& init_ctx) override {
    if (init_ctx.plan) {
      const auto* binding =
          init_ctx.plan->FindPort(name_, PortDirection::kInput);
      if (binding && !binding->blackboard_key.empty()) {
        if (binding->type_id != in_port_.TypeId()) {
          return init_ctx.Fail("Input port type mismatch for '" + name_ + "'");
        }
        in_port_.Resolve(binding->blackboard_key);
      } else {
        if (required_) {
          return init_ctx.Fail("Required input port '" + name_ +
                               "' is unbound in plan");
        }
        in_port_.Unbind();
      }
    } else {
      if (!required_) {
        return init_ctx.Fail(
            "AuthorNode with optional inputs requires a ValidatedNodePlan");
      }
      in_port_.Resolve(name_);
    }
    return true;
  }

  bool PopulateInput(const AlgContext& ctx, InputsT* inputs,
                     std::string* err) const override {
    if (!inputs) return false;
    if (!in_port_.IsBound()) {
      if (required_) {
        if (err) *err = "Required input port '" + name_ + "' is not bound";
        return false;
      }
      inputs->*member_ptr_ = nullptr;
      return true;
    }
    const auto* val = in_port_.Require(const_cast<AlgContext&>(ctx),
                                       node_error::author_node::kMissingInput);
    if (!val) {
      if (err) *err = "Missing required input for port '" + name_ + "'";
      return false;
    }
    inputs->*member_ptr_ = val;
    return true;
  }

  const void* GetRawBatch(const InputsT& inputs) const override {
    return inputs.*member_ptr_;
  }

  bool HasBatch(const InputsT& inputs) const override {
    return (inputs.*member_ptr_) != nullptr;
  }

  TraceableAlignmentResult ValidateAlignment(
      const InputsT& inputs,
      const IProvenanceReader& output_reader) const override {
    const auto* batch = inputs.*member_ptr_;
    if (!batch) {
      return {TraceableAlignmentError::kCountMismatch, 0};
    }
    if (batch->size() != output_reader.Size()) {
      return {TraceableAlignmentError::kCountMismatch,
              std::min(batch->size(), output_reader.Size())};
    }
    for (size_t i = 0; i < batch->size(); ++i) {
      if ((*batch)[i].req_id != output_reader.ReqId(i) ||
          (*batch)[i].sub_id != output_reader.SubId(i)) {
        return {TraceableAlignmentError::kProvenanceMismatch, i};
      }
    }
    return {TraceableAlignmentError::kNone, batch->size()};
  }

  std::unique_ptr<InputPortBinding<InputsT>> Clone() const override {
    return std::make_unique<ConcreteInputPortBinding>(*this);
  }

 private:
  std::string name_;
  MemberPtr member_ptr_;
  bool required_;
  InputFlow flow_;
  BoundInput<BatchT> in_port_;
};

template <typename InputsT>
class InputBindingHolder {
 public:
  InputBindingHolder(std::unique_ptr<InputPortBinding<InputsT>> b)  // NOLINT
      : binding_(std::move(b)) {}

  InputBindingHolder(const InputBindingHolder& other)
      : binding_(other.binding_ ? other.binding_->Clone() : nullptr) {}

  InputBindingHolder(InputBindingHolder&&) noexcept = default;
  InputBindingHolder& operator=(const InputBindingHolder& other) {
    if (this != &other) {
      binding_ = other.binding_ ? other.binding_->Clone() : nullptr;
    }
    return *this;
  }
  InputBindingHolder& operator=(InputBindingHolder&&) noexcept = default;

  const InputPortBinding<InputsT>* get() const noexcept {
    return binding_.get();
  }
  InputPortBinding<InputsT>* get() noexcept { return binding_.get(); }
  const InputPortBinding<InputsT>& operator*() const noexcept {
    return *binding_;
  }
  InputPortBinding<InputsT>& operator*() noexcept { return *binding_; }
  const InputPortBinding<InputsT>* operator->() const noexcept {
    return binding_.get();
  }
  InputPortBinding<InputsT>* operator->() noexcept { return binding_.get(); }

  std::unique_ptr<InputPortBinding<InputsT>> Clone() const {
    return binding_ ? binding_->Clone() : nullptr;
  }

 private:
  std::unique_ptr<InputPortBinding<InputsT>> binding_;
};

template <typename InputsT, typename BatchT>
inline InputBindingHolder<InputsT> Required(
    std::string name, const BatchT* InputsT::*member_ptr,
    InputFlow flow = InputFlow::PreserveByRequest) {
  return InputBindingHolder<InputsT>(
      std::make_unique<ConcreteInputPortBinding<InputsT, BatchT>>(
          std::move(name), member_ptr, true, flow));
}

template <typename InputsT, typename BatchT>
inline InputBindingHolder<InputsT> Optional(
    std::string name, const BatchT* InputsT::*member_ptr,
    InputFlow flow = InputFlow::PreserveByRequest) {
  return InputBindingHolder<InputsT>(
      std::make_unique<ConcreteInputPortBinding<InputsT, BatchT>>(
          std::move(name), member_ptr, false, flow));
}

template <typename InputsT>
class InputsOf {
 public:
  InputsOf(std::initializer_list<InputBindingHolder<InputsT>> bindings) {
    bindings_.reserve(bindings.size());
    for (const auto& b : bindings) {
      bindings_.push_back(b.Clone());
    }
  }

  InputsOf(const InputsOf& other) {
    bindings_.reserve(other.bindings_.size());
    for (const auto& b : other.bindings_) {
      bindings_.push_back(b ? b->Clone() : nullptr);
    }
  }

  InputsOf(InputsOf&&) noexcept = default;
  InputsOf& operator=(const InputsOf& other) {
    if (this != &other) {
      bindings_.clear();
      bindings_.reserve(other.bindings_.size());
      for (const auto& b : other.bindings_) {
        bindings_.push_back(b ? b->Clone() : nullptr);
      }
    }
    return *this;
  }
  InputsOf& operator=(InputsOf&&) noexcept = default;

  std::vector<NodePortDefinition> ToPortDefinitions() const {
    std::vector<NodePortDefinition> defs;
    defs.reserve(bindings_.size());
    for (const auto& b : bindings_) {
      defs.push_back(b->ToPortDefinition());
    }
    return defs;
  }

  bool BindPorts(const NodeInitContext& init_ctx) {
    for (auto& b : bindings_) {
      if (!b->BindPort(init_ctx)) return false;
    }
    return true;
  }

  bool PopulateInputs(const AlgContext& ctx, InputsT* inputs,
                      std::string* err) const {
    for (const auto& b : bindings_) {
      if (!b->PopulateInput(ctx, inputs, err)) return false;
    }
    return true;
  }

  const InputPortBinding<InputsT>* FindPort(const std::string& name) const {
    for (const auto& b : bindings_) {
      if (b->LogicalName() == name) return b.get();
    }
    return nullptr;
  }

  bool HasPort(const std::string& name) const {
    return FindPort(name) != nullptr;
  }

  std::unordered_set<std::string> ConnectedInputs(
      const ValidatedNodePlan* plan) const {
    std::unordered_set<std::string> connected;
    if (plan) {
      for (const auto& p : plan->ports) {
        if (p.direction == PortDirection::kInput && !p.blackboard_key.empty()) {
          connected.insert(p.logical_name);
        }
      }
    } else {
      for (const auto& b : bindings_) {
        if (b->IsRequired()) {
          connected.insert(b->LogicalName());
        }
      }
    }
    return connected;
  }

 private:
  std::vector<std::unique_ptr<InputPortBinding<InputsT>>> bindings_;
};

template <typename OutputBatchT>
struct PreservedOutput {
  static_assert(BatchItemTraits<OutputBatchT>::kIsTraceableBatch,
                "Output batch type must be a vector of TraceableItem<T>");
  using BatchType = OutputBatchT;
  using PayloadType = typename BatchItemTraits<OutputBatchT>::PayloadType;

  std::string output_name;
  std::string anchor_input_name;

  PreservedOutput(std::string out_name, std::string anchor_in_name)
      : output_name(std::move(out_name)),
        anchor_input_name(std::move(anchor_in_name)) {}
};

template <typename ModelsT>
class ModelSlotBinding {
 public:
  virtual ~ModelSlotBinding() = default;
  virtual const std::string& SlotName() const = 0;
  virtual const std::string& ConfigField() const = 0;
  virtual const std::string& Capability() const = 0;
  virtual bool Bind(const NodeInitContext& init_ctx,
                    SessionContext& session_ctx,
                    const nlohmann::json& normalized_config, ModelsT* models,
                    std::string* err) = 0;
  virtual std::unique_ptr<ModelSlotBinding<ModelsT>> Clone() const = 0;
};

template <typename ModelsT>
class LlmModelSlotBinding final : public ModelSlotBinding<ModelsT> {
 public:
  using MemberPtr = LlmCall ModelsT::*;

  LlmModelSlotBinding(std::string slot_name, std::string config_field,
                      MemberPtr member_ptr)
      : slot_name_(std::move(slot_name)),
        config_field_(std::move(config_field)),
        member_ptr_(member_ptr) {}

  const std::string& SlotName() const override { return slot_name_; }
  const std::string& ConfigField() const override { return config_field_; }
  const std::string& Capability() const override {
    static const std::string cap = "llm";
    return cap;
  }

  bool Bind(const NodeInitContext& init_ctx, SessionContext& session_ctx,
            const nlohmann::json& config, ModelsT* models,
            std::string* err) override {
    std::string model_id;
    if (init_ctx.plan) {
      const auto* binding = init_ctx.plan->FindModelBinding(slot_name_);
      if (!binding || binding->model_id.empty()) {
        if (err) {
          *err = "Model binding for '" + slot_name_ +
                 "' is missing or empty in plan";
        }
        return false;
      }
      if (binding->capability != Capability()) {
        if (err) {
          *err = "Model binding capability mismatch for '" + slot_name_ + "'";
        }
        return false;
      }
      model_id = binding->model_id;
    } else {
      if (!config.contains(config_field_) ||
          !config[config_field_].is_string()) {
        if (err) *err = "Config missing model field: " + config_field_;
        return false;
      }
      model_id = config[config_field_].template get<std::string>();
      if (model_id.empty()) {
        if (err) *err = "Model binding field '" + config_field_ + "' is empty";
        return false;
      }
    }
    auto model = session_ctx.GetModelManager().GetModel<ILlmModel>(model_id);
    if (!model) {
      if (err) {
        *err = "LLM model '" + model_id + "' is unavailable or incompatible";
      }
      return false;
    }
    models->*member_ptr_ = LlmCall(std::move(model), slot_name_);
    return true;
  }

  std::unique_ptr<ModelSlotBinding<ModelsT>> Clone() const override {
    return std::make_unique<LlmModelSlotBinding>(*this);
  }

 private:
  std::string slot_name_;
  std::string config_field_;
  MemberPtr member_ptr_;
};

template <typename ModelsT>
class EmbeddingModelSlotBinding final : public ModelSlotBinding<ModelsT> {
 public:
  using MemberPtr = EmbeddingCall ModelsT::*;

  EmbeddingModelSlotBinding(std::string slot_name, std::string config_field,
                            MemberPtr member_ptr)
      : slot_name_(std::move(slot_name)),
        config_field_(std::move(config_field)),
        member_ptr_(member_ptr) {}

  const std::string& SlotName() const override { return slot_name_; }
  const std::string& ConfigField() const override { return config_field_; }
  const std::string& Capability() const override {
    static const std::string cap = "embedding";
    return cap;
  }

  bool Bind(const NodeInitContext& init_ctx, SessionContext& session_ctx,
            const nlohmann::json& config, ModelsT* models,
            std::string* err) override {
    std::string model_id;
    if (init_ctx.plan) {
      const auto* binding = init_ctx.plan->FindModelBinding(slot_name_);
      if (!binding || binding->model_id.empty()) {
        if (err) {
          *err = "Model binding for '" + slot_name_ +
                 "' is missing or empty in plan";
        }
        return false;
      }
      if (binding->capability != Capability()) {
        if (err) {
          *err = "Model binding capability mismatch for '" + slot_name_ + "'";
        }
        return false;
      }
      model_id = binding->model_id;
    } else {
      if (!config.contains(config_field_) ||
          !config[config_field_].is_string()) {
        if (err) *err = "Config missing model field: " + config_field_;
        return false;
      }
      model_id = config[config_field_].template get<std::string>();
      if (model_id.empty()) {
        if (err) *err = "Model binding field '" + config_field_ + "' is empty";
        return false;
      }
    }
    auto model =
        session_ctx.GetModelManager().GetModel<IEmbeddingModel>(model_id);
    if (!model) {
      if (err) {
        *err =
            "Embedding model '" + model_id + "' is unavailable or incompatible";
      }
      return false;
    }
    models->*member_ptr_ = EmbeddingCall(std::move(model), slot_name_);
    return true;
  }

  std::unique_ptr<ModelSlotBinding<ModelsT>> Clone() const override {
    return std::make_unique<EmbeddingModelSlotBinding>(*this);
  }

 private:
  std::string slot_name_;
  std::string config_field_;
  MemberPtr member_ptr_;
};

template <typename ModelsT>
class ModelSlotBindingHolder {
 public:
  ModelSlotBindingHolder(
      std::unique_ptr<ModelSlotBinding<ModelsT>> b)  // NOLINT
      : binding_(std::move(b)) {}

  ModelSlotBindingHolder(const ModelSlotBindingHolder& other)
      : binding_(other.binding_ ? other.binding_->Clone() : nullptr) {}

  ModelSlotBindingHolder(ModelSlotBindingHolder&&) noexcept = default;
  ModelSlotBindingHolder& operator=(const ModelSlotBindingHolder& other) {
    if (this != &other) {
      binding_ = other.binding_ ? other.binding_->Clone() : nullptr;
    }
    return *this;
  }
  ModelSlotBindingHolder& operator=(ModelSlotBindingHolder&&) noexcept =
      default;

  const ModelSlotBinding<ModelsT>* get() const noexcept {
    return binding_.get();
  }
  ModelSlotBinding<ModelsT>* get() noexcept { return binding_.get(); }
  const ModelSlotBinding<ModelsT>& operator*() const noexcept {
    return *binding_;
  }
  ModelSlotBinding<ModelsT>& operator*() noexcept { return *binding_; }
  const ModelSlotBinding<ModelsT>* operator->() const noexcept {
    return binding_.get();
  }
  ModelSlotBinding<ModelsT>* operator->() noexcept { return binding_.get(); }

  std::unique_ptr<ModelSlotBinding<ModelsT>> Clone() const {
    return binding_ ? binding_->Clone() : nullptr;
  }

 private:
  std::unique_ptr<ModelSlotBinding<ModelsT>> binding_;
};

template <typename ModelsT>
inline ModelSlotBindingHolder<ModelsT> Llm(std::string slot_name,
                                           std::string config_field,
                                           LlmCall ModelsT::*member_ptr) {
  return ModelSlotBindingHolder<ModelsT>(
      std::make_unique<LlmModelSlotBinding<ModelsT>>(
          std::move(slot_name), std::move(config_field), member_ptr));
}

template <typename ModelsT>
inline ModelSlotBindingHolder<ModelsT> Embedding(
    std::string slot_name, std::string config_field,
    EmbeddingCall ModelsT::*member_ptr) {
  return ModelSlotBindingHolder<ModelsT>(
      std::make_unique<EmbeddingModelSlotBinding<ModelsT>>(
          std::move(slot_name), std::move(config_field), member_ptr));
}

template <typename ModelsT>
class ModelsOf {
 public:
  ModelsOf() = default;

  ModelsOf(std::initializer_list<ModelSlotBindingHolder<ModelsT>> bindings) {
    std::unordered_set<std::string> seen_slots;
    std::unordered_set<std::string> seen_fields;
    bindings_.reserve(bindings.size());
    for (const auto& b : bindings) {
      if (!b.get()) continue;
      const auto& slot = b->SlotName();
      const auto& field = b->ConfigField();
      if (seen_slots.count(slot) > 0) {
        throw std::invalid_argument("Duplicate model slot name: " + slot);
      }
      if (seen_fields.count(field) > 0) {
        throw std::invalid_argument("Duplicate model config field: " + field);
      }
      seen_slots.insert(slot);
      seen_fields.insert(field);
      bindings_.push_back(b.Clone());
    }
  }

  ModelsOf(const ModelsOf& other) {
    bindings_.reserve(other.bindings_.size());
    for (const auto& b : other.bindings_) {
      bindings_.push_back(b ? b->Clone() : nullptr);
    }
  }

  ModelsOf(ModelsOf&&) noexcept = default;
  ModelsOf& operator=(const ModelsOf& other) {
    if (this != &other) {
      bindings_.clear();
      bindings_.reserve(other.bindings_.size());
      for (const auto& b : other.bindings_) {
        bindings_.push_back(b ? b->Clone() : nullptr);
      }
    }
    return *this;
  }
  ModelsOf& operator=(ModelsOf&&) noexcept = default;

  std::vector<ConfigFieldDefinition> ToConfigFields() const {
    std::vector<ConfigFieldDefinition> fields;
    fields.reserve(bindings_.size());
    for (const auto& b : bindings_) {
      fields.push_back(ConfigFieldDefinition{
          b->ConfigField(),
          ConfigValueKind::kString,
          true,
          nlohmann::json(),
          std::nullopt,
          std::nullopt,
          {},
          "Bound model ID for slot '" + b->SlotName() + "'"});
    }
    return fields;
  }

  bool BindModels(const NodeInitContext& init_ctx, SessionContext& session_ctx,
                  const nlohmann::json& config, ModelsT* models,
                  std::string* err) {
    for (auto& b : bindings_) {
      if (!b->Bind(init_ctx, session_ctx, config, models, err)) return false;
    }
    return true;
  }

  const std::vector<std::unique_ptr<ModelSlotBinding<ModelsT>>>& Bindings()
      const noexcept {
    return bindings_;
  }

 private:
  std::vector<std::unique_ptr<ModelSlotBinding<ModelsT>>> bindings_;
};

template <>
class ModelsOf<NoModels> {
 public:
  std::vector<ConfigFieldDefinition> ToConfigFields() const { return {}; }
  bool BindModels(const NodeInitContext&, SessionContext&,
                  const nlohmann::json&, NoModels*, std::string*) {
    return true;
  }
};

template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
class BatchSpec {
 public:
  using InputsType = InputsT;
  using OutputBatch = OutputBatchT;
  using ParametersType = ParamsT;
  using ModelsType = ModelsT;
  using RunFunctionType = RunFnT;

  BatchSpec(InputsOf<InputsT> inputs, PreservedOutput<OutputBatchT> output,
            Parameters<ParamsT> params, ModelsOf<ModelsT> models, RunFnT fn)
      : inputs_(std::move(inputs)),
        output_(std::move(output)),
        params_(std::move(params)),
        models_(std::move(models)),
        fn_(std::move(fn)) {
    if (!inputs_.HasPort(output_.anchor_input_name)) {
      throw std::invalid_argument("PreservedOutput anchor port '" +
                                  output_.anchor_input_name +
                                  "' not found in declared inputs");
    }
  }

  BatchSpec& Description(std::string desc) & {
    description_ = std::move(desc);
    return *this;
  }
  BatchSpec Description(std::string desc) && {
    description_ = std::move(desc);
    return std::move(*this);
  }

  BatchSpec& Category(std::string cat) & {
    category_ = std::move(cat);
    return *this;
  }
  BatchSpec Category(std::string cat) && {
    category_ = std::move(cat);
    return std::move(*this);
  }

  BatchSpec& ParallelSafe(bool safe) & {
    parallel_safe_ = safe;
    return *this;
  }
  BatchSpec ParallelSafe(bool safe) && {
    parallel_safe_ = safe;
    return std::move(*this);
  }

  BatchSpec& BizNames(std::vector<std::string> biz_names) & {
    biz_names_ = std::move(biz_names);
    return *this;
  }
  BatchSpec BizNames(std::vector<std::string> biz_names) && {
    biz_names_ = std::move(biz_names);
    return std::move(*this);
  }

  InputsOf<InputsT>& Inputs() noexcept { return inputs_; }
  const InputsOf<InputsT>& Inputs() const noexcept { return inputs_; }
  const PreservedOutput<OutputBatchT>& Output() const noexcept {
    return output_;
  }
  const Parameters<ParamsT>& ParametersSpec() const noexcept { return params_; }
  ModelsOf<ModelsT>& Models() noexcept { return models_; }
  const ModelsOf<ModelsT>& Models() const noexcept { return models_; }
  const RunFnT& Function() const noexcept { return fn_; }

  std::vector<ConfigFieldDefinition> MergedFields() const {
    auto fields = params_.Fields();
    auto model_fields = models_.ToConfigFields();
    fields.insert(fields.end(), model_fields.begin(), model_fields.end());
    return fields;
  }

  NodeDefinition BuildDefinition(std::string node_type) const {
    NodeDefinition def;
    def.node_type = std::move(node_type);
    def.category = category_;
    def.description = description_;
    def.parallel_safe = parallel_safe_;
    def.biz_names = biz_names_;
    def.inputs = inputs_.ToPortDefinitions();
    def.outputs = {NodePortDefinition{
        output_.output_name, BlackboardTypeTraits<OutputBatchT>::TypeName(),
        true, "1:1", "preserve", "request"}};

    def.config_fields = MergedFields();

    if constexpr (!std::is_same_v<ModelsT, NoModels>) {
      for (const auto& b : models_.Bindings()) {
        def.model_dependencies.push_back(NodeModelDependency{
            b->SlotName(),
            b->Capability(),
            b->ConfigField(),
        });
      }
    }

    def.validate_config = [params = params_](
                              const nlohmann::json& cfg,
                              const std::unordered_set<std::string>& conn,
                              std::string* err) {
      return params.ValidateWithBindings(cfg, conn, err);
    };
    return def;
  }

 private:
  InputsOf<InputsT> inputs_;
  PreservedOutput<OutputBatchT> output_;
  Parameters<ParamsT> params_;
  ModelsOf<ModelsT> models_;
  RunFnT fn_;
  std::string category_ = "custom";
  std::string description_;
  bool parallel_safe_ = false;
  std::vector<std::string> biz_names_;
};

// Batch overloads
template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          PreservedOutput<OutputBatchT> output,
                          Parameters<ParamsT> params, ModelsOf<ModelsT> models,
                          RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>(
      std::move(inputs), std::move(output), std::move(params),
      std::move(models), std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename ModelsT,
          typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          PreservedOutput<OutputBatchT> output,
                          ModelsOf<ModelsT> models, RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, NoParameters, ModelsT, RunFnT>(
      std::move(inputs), std::move(output), Parameters<NoParameters>{},
      std::move(models), std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          PreservedOutput<OutputBatchT> output,
                          Parameters<ParamsT> params, RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, ParamsT, NoModels, RunFnT>(
      std::move(inputs), std::move(output), std::move(params),
      ModelsOf<NoModels>{}, std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          PreservedOutput<OutputBatchT> output, RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, NoParameters, NoModels, RunFnT>(
      std::move(inputs), std::move(output), Parameters<NoParameters>{},
      ModelsOf<NoModels>{}, std::move(fn));
}

// ---------------------------------------------------------------------------
// AuthorNode definition: supports both MapSpec and BatchSpec
// ---------------------------------------------------------------------------

template <typename SpecT, typename = void>
class AuthorNode;

// MapSpec Specialization
template <typename InputBatchT, typename OutputBatchT, typename ParamsT,
          typename MapFnT>
class AuthorNode<MapSpec<InputBatchT, OutputBatchT, ParamsT, MapFnT>>
    : public NodeBase {
 public:
  using SpecType = MapSpec<InputBatchT, OutputBatchT, ParamsT, MapFnT>;

  AuthorNode(std::string node_name, SpecType spec)
      : NodeBase(std::move(node_name)),
        spec_(std::move(spec)),
        in_port_(spec_.InputName()),
        out_port_(spec_.OutputName()) {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& session_ctx) override {
    (void)session_ctx;
    if (init_ctx.plan) {
      const auto* in_binding =
          init_ctx.plan->FindPort(spec_.InputName(), PortDirection::kInput);
      if (!in_binding || in_binding->blackboard_key.empty()) {
        return init_ctx.Fail("Required input port '" + spec_.InputName() +
                             "' has no binding in plan");
      }
      if (in_binding->type_id != in_port_.TypeId()) {
        return init_ctx.Fail("Input port type mismatch for '" +
                             spec_.InputName() +
                             "' (expected: " + in_port_.TypeId() +
                             ", bound: " + in_binding->type_id + ")");
      }
      in_port_.Resolve(in_binding->blackboard_key);

      const auto* out_binding =
          init_ctx.plan->FindPort(spec_.OutputName(), PortDirection::kOutput);
      if (!out_binding || out_binding->blackboard_key.empty()) {
        return init_ctx.Fail("Output port '" + spec_.OutputName() +
                             "' has no binding in plan");
      }
      if (out_binding->type_id != out_port_.TypeId()) {
        return init_ctx.Fail("Output port type mismatch for '" +
                             spec_.OutputName() +
                             "' (expected: " + out_port_.TypeId() +
                             ", bound: " + out_binding->type_id + ")");
      }
      out_port_.Resolve(out_binding->blackboard_key);
    } else {
      in_port_.Resolve(spec_.InputName());
      out_port_.Resolve(spec_.OutputName());
    }

    nlohmann::json normalized;
    if (init_ctx.plan && !init_ctx.plan->normalized_config.is_null() &&
        !init_ctx.plan->normalized_config.empty()) {
      normalized = init_ctx.plan->normalized_config;
    } else {
      std::vector<ConfigFieldValidationError> validation_errors;
      if (!ValidateAndNormalizeFields(spec_.ParametersSpec().Fields(), config,
                                      &normalized, &validation_errors)) {
        return init_ctx.Fail(validation_errors.empty()
                                 ? "Invalid configuration"
                                 : validation_errors.front().message);
      }
    }

    std::unordered_set<std::string> connected_inputs;
    if (init_ctx.plan) {
      for (const auto& p : init_ctx.plan->ports) {
        if (p.direction == PortDirection::kInput && !p.blackboard_key.empty()) {
          connected_inputs.insert(p.logical_name);
        }
      }
    } else {
      connected_inputs.insert(spec_.InputName());
    }

    std::string err;
    if (!spec_.ParametersSpec().ValidateWithBindings(normalized,
                                                     connected_inputs, &err)) {
      return init_ctx.Fail(err.empty() ? "Invalid node configuration" : err);
    }
    auto parsed = spec_.ParametersSpec().ParseNormalized(normalized, &err);
    if (!parsed) {
      return init_ctx.Fail(err.empty() ? "Invalid node configuration" : err);
    }
    parameters_ = std::move(*parsed);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    using OutputBatch = typename SpecType::OutputBatch;

    const auto* inputs =
        in_port_.Require(req_ctx, node_error::author_node::kMissingInput);
    if (!inputs) {
      return node_error::author_node::kMissingInput;
    }

    if (inputs->empty()) {
      out_port_.Set(req_ctx, OutputBatch{});
      return 0;
    }

    OutputBatch outputs;
    outputs.reserve(inputs->size());

    for (const auto& item : *inputs) {
      if constexpr (SpecType::kReturnsNodeResult) {
        auto res =
            detail::InvokeMapItem(spec_.Function(), item.data, parameters_);
        if (!res.ok()) {
          auto failure = std::move(res).ExtractFailure();
          int code = failure.cause_code != 0
                         ? failure.cause_code
                         : node_error::author_node::kBusinessError;
          return this->Fail(req_ctx, code,
                            failure.message.empty()
                                ? (this->Name() + " map function failed")
                                : failure.message);
        }
        outputs.emplace_back(item.req_id, item.sub_id, std::move(res).value());
      } else {
        outputs.emplace_back(
            item.req_id, item.sub_id,
            detail::InvokeMapItem(spec_.Function(), item.data, parameters_));
      }
    }

    const auto alignment =
        ValidatePreservedTraceableAlignment(*inputs, outputs);
    if (alignment.error == TraceableAlignmentError::kCountMismatch) {
      return this->Fail(req_ctx, node_error::author_node::kOutputCountMismatch,
                        this->Name() + " output count mismatch");
    }
    if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
      return this->Fail(req_ctx,
                        node_error::author_node::kOutputProvenanceMismatch,
                        this->Name() + " output provenance mismatch");
    }

    out_port_.Set(req_ctx, std::move(outputs));
    return 0;
  }

 private:
  SpecType spec_;
  BoundInput<typename SpecType::InputBatch> in_port_;
  BoundOutput<typename SpecType::OutputBatch> out_port_;
  typename SpecType::ParametersType parameters_{};
};

// BatchSpec Specialization
template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
class AuthorNode<BatchSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>>
    : public NodeBase {
 public:
  using SpecType = BatchSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>;

  AuthorNode(std::string node_name, SpecType spec)
      : NodeBase(std::move(node_name)),
        spec_(std::move(spec)),
        out_port_(spec_.Output().output_name) {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& session_ctx) override {
    if (!spec_.Inputs().BindPorts(init_ctx)) {
      return false;
    }

    if (init_ctx.plan) {
      const auto* out_binding = init_ctx.plan->FindPort(
          spec_.Output().output_name, PortDirection::kOutput);
      if (!out_binding || out_binding->blackboard_key.empty()) {
        return init_ctx.Fail("Output port '" + spec_.Output().output_name +
                             "' has no binding in plan");
      }
      if (out_binding->type_id != out_port_.TypeId()) {
        return init_ctx.Fail("Output port type mismatch for '" +
                             spec_.Output().output_name + "'");
      }
      out_port_.Resolve(out_binding->blackboard_key);
    } else {
      out_port_.Resolve(spec_.Output().output_name);
    }

    nlohmann::json normalized;
    if (init_ctx.plan && !init_ctx.plan->normalized_config.is_null() &&
        !init_ctx.plan->normalized_config.empty()) {
      normalized = init_ctx.plan->normalized_config;
    } else {
      std::vector<ConfigFieldValidationError> validation_errors;
      if (!ValidateAndNormalizeFields(spec_.MergedFields(), config, &normalized,
                                      &validation_errors)) {
        return init_ctx.Fail(validation_errors.empty()
                                 ? "Invalid configuration"
                                 : validation_errors.front().message);
      }
    }

    auto connected_inputs = spec_.Inputs().ConnectedInputs(init_ctx.plan);
    std::string err;
    if (!spec_.ParametersSpec().ValidateWithBindings(normalized,
                                                     connected_inputs, &err)) {
      return init_ctx.Fail(err.empty() ? "Invalid node configuration" : err);
    }

    auto parsed = spec_.ParametersSpec().ParseNormalized(normalized, &err);
    if (!parsed) {
      return init_ctx.Fail(err.empty() ? "Invalid node configuration" : err);
    }
    parameters_ = std::move(*parsed);

    if (!spec_.Models().BindModels(init_ctx, session_ctx, normalized, &models_,
                                   &err)) {
      return init_ctx.Fail(err.empty() ? "Failed to bind models" : err);
    }

    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    InputsT inputs{};
    std::string err;
    if (!spec_.Inputs().PopulateInputs(req_ctx, &inputs, &err)) {
      return this->Fail(req_ctx, node_error::author_node::kMissingInput,
                        err.empty() ? "Failed to populate inputs" : err);
    }

    auto res =
        detail::InvokeBatch(spec_.Function(), inputs, parameters_, models_);
    if (!res.ok()) {
      auto failure = std::move(res).ExtractFailure();
      int code = failure.cause_code != 0
                     ? failure.cause_code
                     : node_error::author_node::kBusinessError;
      return this->Fail(req_ctx, code,
                        failure.message.empty()
                            ? (this->Name() + " process failed")
                            : failure.message);
    }

    OutputBatchT output = std::move(res).value();

    // Anchor check
    const auto* anchor_port =
        spec_.Inputs().FindPort(spec_.Output().anchor_input_name);
    if (!anchor_port) {
      return this->Fail(req_ctx, node_error::author_node::kMissingOutputAnchor,
                        "Anchor input port not found");
    }

    if (!anchor_port->HasBatch(inputs)) {
      // If anchor was unconnected/null, only empty output is accepted
      if (!output.empty()) {
        return this->Fail(req_ctx,
                          node_error::author_node::kOutputCountMismatch,
                          "Output produced without anchor input");
      }
    } else {
      TraceableBatchProvenanceReader<OutputBatchT> reader(output);
      auto alignment = anchor_port->ValidateAlignment(inputs, reader);
      if (alignment.error == TraceableAlignmentError::kCountMismatch) {
        return this->Fail(req_ctx,
                          node_error::author_node::kOutputCountMismatch,
                          this->Name() + " output count mismatch");
      }
      if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
        return this->Fail(req_ctx,
                          node_error::author_node::kOutputProvenanceMismatch,
                          this->Name() + " output provenance mismatch");
      }
    }

    out_port_.Set(req_ctx, std::move(output));
    return 0;
  }

 private:
  SpecType spec_;
  BoundOutput<OutputBatchT> out_port_;
  typename SpecType::ParametersType parameters_{};
  typename SpecType::ModelsType models_{};
};

// ---------------------------------------------------------------------------
// LLM Text Shortcut Spec Factory (RFC 0052 Section 4.4)
// ---------------------------------------------------------------------------

struct LlmTextInputs {
  const TextBatch* prompt = nullptr;
};

struct LlmTextModels {
  LlmCall generator;
};

template <typename ParamsT, typename BuildPromptFn, typename FormatAnswerFn>
inline auto MakeLlmTextSpec(Input<TextBatch> in_port,
                            Output<TextBatch> out_port,
                            Parameters<ParamsT> params,
                            BuildPromptFn build_prompt,
                            FormatAnswerFn format_answer,
                            GenerateOptions options = GenerateOptions{}) {
  auto run_fn = [build_prompt = std::move(build_prompt),
                 format_answer = std::move(format_answer),
                 options = std::move(options)](
                    const LlmTextInputs& inputs, const ParamsT& parameters,
                    const LlmTextModels& models) -> NodeResult<TextBatch> {
    if (!inputs.prompt || inputs.prompt->empty()) {
      return NodeResult<TextBatch>::Success(TextBatch{});
    }

    // Step 1: Build prompts
    TextBatch prompts;
    prompts.reserve(inputs.prompt->size());
    for (const auto& item : *inputs.prompt) {
      if constexpr (std::is_invocable_v<BuildPromptFn, const std::string&,
                                        const ParamsT&>) {
        auto res = build_prompt(item.data, parameters);
        if constexpr (IsNodeResult<decltype(res)>::value) {
          if (!res.ok()) {
            return NodeResult<TextBatch>::Failure(
                std::move(res).ExtractFailure());
          }
          prompts.emplace_back(item.req_id, item.sub_id,
                               std::move(res).value());
        } else {
          prompts.emplace_back(item.req_id, item.sub_id, std::move(res));
        }
      } else {
        auto res = build_prompt(item.data);
        if constexpr (IsNodeResult<decltype(res)>::value) {
          if (!res.ok()) {
            return NodeResult<TextBatch>::Failure(
                std::move(res).ExtractFailure());
          }
          prompts.emplace_back(item.req_id, item.sub_id,
                               std::move(res).value());
        } else {
          prompts.emplace_back(item.req_id, item.sub_id, std::move(res));
        }
      }
    }

    // Step 2: Call LLM
    auto llm_res = models.generator.Generate(prompts, options);
    if (!llm_res.ok()) {
      return llm_res;
    }

    // Format the owned model output in place; nothing is published until
    // success.
    auto outputs = std::move(llm_res).value();
    for (auto& item : outputs) {
      if constexpr (std::is_invocable_v<FormatAnswerFn, const std::string&,
                                        const ParamsT&>) {
        auto res = format_answer(std::as_const(item.data), parameters);
        if constexpr (IsNodeResult<decltype(res)>::value) {
          if (!res.ok()) {
            return NodeResult<TextBatch>::Failure(
                std::move(res).ExtractFailure());
          }
          item.data = std::move(res).value();
        } else {
          item.data = std::move(res);
        }
      } else {
        auto res = format_answer(std::as_const(item.data));
        if constexpr (IsNodeResult<decltype(res)>::value) {
          if (!res.ok()) {
            return NodeResult<TextBatch>::Failure(
                std::move(res).ExtractFailure());
          }
          item.data = std::move(res).value();
        } else {
          item.data = std::move(res);
        }
      }
    }

    return NodeResult<TextBatch>::Success(std::move(outputs));
  };

  return MakeBatchSpec(
      InputsOf<LlmTextInputs>({
          Required(in_port.name, &LlmTextInputs::prompt),
      }),
      PreservedOutput<TextBatch>(out_port.name, in_port.name),
      std::move(params),
      ModelsOf<LlmTextModels>({
          Llm("generator", "bind_model", &LlmTextModels::generator),
      }),
      std::move(run_fn));
}

template <typename ParamsT, typename BuildPromptFn, typename FormatAnswerFn>
inline auto MakeLlmTextSpec(Parameters<ParamsT> params,
                            BuildPromptFn build_prompt,
                            FormatAnswerFn format_answer,
                            GenerateOptions options = GenerateOptions{}) {
  return MakeLlmTextSpec(Input<TextBatch>("prompt"), Output<TextBatch>("text"),
                         std::move(params), std::move(build_prompt),
                         std::move(format_answer), std::move(options));
}

template <typename BuildPromptFn, typename FormatAnswerFn>
inline auto MakeLlmTextSpec(BuildPromptFn build_prompt,
                            FormatAnswerFn format_answer,
                            GenerateOptions options = GenerateOptions{}) {
  return MakeLlmTextSpec(Input<TextBatch>("prompt"), Output<TextBatch>("text"),
                         Parameters<NoParameters>{}, std::move(build_prompt),
                         std::move(format_answer), std::move(options));
}

template <typename BuildPromptFn, typename FormatAnswerFn>
inline auto MakeLlmTextSpec(Input<TextBatch> in_port,
                            Output<TextBatch> out_port,
                            BuildPromptFn build_prompt,
                            FormatAnswerFn format_answer,
                            GenerateOptions options = GenerateOptions{}) {
  return MakeLlmTextSpec(std::move(in_port), std::move(out_port),
                         Parameters<NoParameters>{}, std::move(build_prompt),
                         std::move(format_answer), std::move(options));
}

#define REGISTER_FUNCTION_NODE(NodeType, ...)                              \
  struct NodeType final : public ::llm_edgeflow::AuthorNode<               \
                              std::decay_t<decltype(__VA_ARGS__)>> {       \
    static constexpr const char* kNodeType = #NodeType;                    \
    NodeType()                                                             \
        : ::llm_edgeflow::AuthorNode<std::decay_t<decltype(__VA_ARGS__)>>( \
              #NodeType, (__VA_ARGS__)) {}                                 \
  };                                                                       \
  REGISTER_NODE_WITH_DEFINITION(NodeType, []() {                           \
    auto spec = (__VA_ARGS__);                                             \
    return spec.BuildDefinition(#NodeType);                                \
  }())

}  // namespace llm_edgeflow
