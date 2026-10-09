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
#include "contracts/parameters.h"
#include "contracts/traceable_item.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "core/port_definition.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "engine/model_capability_traits.h"
#include "nodes/configuration_snapshot.h"
#include "nodes/control_authoring.h"
#include "nodes/model_binding.h"
#include "nodes/model_calls.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"
#include "nodes/session_resources.h"
#include "nodes/traceable_algorithms.h"
#include "nodes/traceable_batch_validation.h"

namespace llm_edgeflow {

template <typename T>
struct IsNodeResult : std::false_type {
  using ValueType = T;
};

template <typename T>
struct IsNodeResult<NodeResult<T>> : std::true_type {
  using ValueType = T;
};

namespace detail {

template <typename ParamsT>
inline constexpr bool kHasParameters = !std::is_same_v<ParamsT, NoParameters>;
template <typename ModelsT>
inline constexpr bool kHasModels = !std::is_same_v<ModelsT, NoModels>;

// Run 的参数依次为 Inputs、Params、Models，只包含 Spec 实际声明的部分；
// 需要会话缓存时再追加 SessionResources。
template <typename Fn, typename InputsT, typename ParamsT, typename ModelsT,
          typename... Tail>
inline constexpr bool kRunInvocable = [] {
  if constexpr (kHasParameters<ParamsT> && kHasModels<ModelsT>) {
    return std::is_invocable_v<const Fn&, const InputsT&, const ParamsT&,
                               const ModelsT&, Tail...>;
  } else if constexpr (kHasParameters<ParamsT>) {
    return std::is_invocable_v<const Fn&, const InputsT&, const ParamsT&,
                               Tail...>;
  } else if constexpr (kHasModels<ModelsT>) {
    return std::is_invocable_v<const Fn&, const InputsT&, const ModelsT&,
                               Tail...>;
  } else {
    return std::is_invocable_v<const Fn&, const InputsT&, Tail...>;
  }
}();

template <typename Fn, typename InputsT, typename ParamsT, typename ModelsT,
          typename... Tail>
auto CallRun(const Fn& fn, const InputsT& inputs, const ParamsT& params,
             const ModelsT& models, const Tail&... tail) {
  if constexpr (kHasParameters<ParamsT> && kHasModels<ModelsT>) {
    return fn(inputs, params, models, tail...);
  } else if constexpr (kHasParameters<ParamsT>) {
    return fn(inputs, params, tail...);
  } else if constexpr (kHasModels<ModelsT>) {
    return fn(inputs, models, tail...);
  } else {
    return fn(inputs, tail...);
  }
}

template <typename Fn, typename InputsT, typename ParamsT, typename ModelsT>
auto InvokeRun(const Fn& fn, const InputsT& inputs, const ParamsT& params,
               const ModelsT& models, const SessionResources& resources) {
  if constexpr (kRunInvocable<Fn, InputsT, ParamsT, ModelsT,
                              const SessionResources&>) {
    return CallRun(fn, inputs, params, models, resources);
  } else if constexpr (kRunInvocable<Fn, InputsT, ParamsT, ModelsT>) {
    return CallRun(fn, inputs, params, models);
  }
}

template <typename Fn, typename InputsT, typename ParamsT, typename ModelsT>
struct RunSignature {
  static constexpr bool kCallable =
      kRunInvocable<Fn, InputsT, ParamsT, ModelsT, const SessionResources&> ||
      kRunInvocable<Fn, InputsT, ParamsT, ModelsT>;
  // 无签名匹配时 InvokeRun 返回 void，且不会实例化非法调用。
  using Result = decltype(InvokeRun(
      std::declval<const Fn&>(), std::declval<const InputsT&>(),
      std::declval<const ParamsT&>(), std::declval<const ModelsT&>(),
      std::declval<const SessionResources&>()));
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Node Spec
// ---------------------------------------------------------------------------

enum class InputFlow {
  PreserveByRequest,
  AggregateByRequest,
};

// 流元数据描述现有的端口契约，不转换数据。
struct PortFlow {
  std::string cardinality = "1:1";
  std::string provenance = "preserve";
  std::string lifetime = "request";
  std::string lifetime_config_field;

  PortFlow() = default;
  PortFlow(std::string card, std::string prov, std::string life = "request",
           std::string lifetime_field = {})
      : cardinality(std::move(card)),
        provenance(std::move(prov)),
        lifetime(std::move(life)),
        lifetime_config_field(std::move(lifetime_field)) {}
  PortFlow(InputFlow flow)  // NOLINT
      : cardinality(flow == InputFlow::AggregateByRequest ? "N:1" : "1:1"),
        provenance(flow == InputFlow::AggregateByRequest ? "aggregate"
                                                         : "preserve") {}
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
                           bool required, PortFlow flow,
                           bool allow_missing_value = false)
      : name_(std::move(name)),
        member_ptr_(member_ptr),
        required_(required),
        flow_(std::move(flow)),
        allow_missing_value_(allow_missing_value),
        in_port_(name_) {}

  const std::string& LogicalName() const override { return name_; }
  bool IsRequired() const override { return required_; }

  NodePortDefinition ToPortDefinition() const override {
    return NodePortDefinition{name_,
                              BlackboardTypeTraits<BatchT>::TypeName(),
                              required_,
                              flow_.cardinality,
                              flow_.provenance,
                              flow_.lifetime,
                              flow_.lifetime_config_field};
  }

  bool BindPort(const NodeInitContext& init_ctx) override {
    const auto result = detail::ResolvePortBinding(
        *init_ctx.plan, PortDirection::kInput, in_port_);
    if (result.status != detail::PortBindingStatus::kUnbound) {
      if (result.status == detail::PortBindingStatus::kTypeMismatch) {
        return init_ctx.Fail("Input port type mismatch for '" + name_ +
                             "' (expected: " + in_port_.TypeId() +
                             ", bound: " + result.binding->type_id + ")");
      }
    } else {
      if (required_) {
        return init_ctx.Fail("Required input port '" + name_ +
                             "' has no binding in plan");
      }
      in_port_.Unbind();
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
    const auto* val = in_port_.Get(ctx);
    if (!val && in_port_.Has(ctx)) {
      if (err) {
        *err = "Input port type mismatch for '" + name_ + "' (bound key: '" +
               in_port_.ActualKey() + "') expected type: " + in_port_.TypeId();
      }
      return false;
    }
    if (allow_missing_value_) {
      inputs->*member_ptr_ = val;
      return true;
    }
    if (!val) {
      if (err) *err = "Missing required input for port '" + name_ + "'";
      return false;
    }
    inputs->*member_ptr_ = val;
    return true;
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
  PortFlow flow_;
  bool allow_missing_value_;
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
inline InputBindingHolder<InputsT> Required(std::string name,
                                            const BatchT* InputsT::*member_ptr,
                                            PortFlow flow = {}) {
  return InputBindingHolder<InputsT>(
      std::make_unique<ConcreteInputPortBinding<InputsT, BatchT>>(
          std::move(name), member_ptr, true, flow));
}

template <typename InputsT, typename BatchT>
inline InputBindingHolder<InputsT> Optional(std::string name,
                                            const BatchT* InputsT::*member_ptr,
                                            PortFlow flow = {}) {
  return InputBindingHolder<InputsT>(
      std::make_unique<ConcreteInputPortBinding<InputsT, BatchT>>(
          std::move(name), member_ptr, false, flow));
}

// 可选连接：请求中缺失的值由算法处理
// (例如模板缺失变量策略或按条件使用的上下文)。
template <typename InputsT, typename BatchT>
inline InputBindingHolder<InputsT> OptionalValue(
    std::string name, const BatchT* InputsT::*member_ptr, PortFlow flow = {}) {
  return InputBindingHolder<InputsT>(
      std::make_unique<ConcreteInputPortBinding<InputsT, BatchT>>(
          std::move(name), member_ptr, false, std::move(flow), true));
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

 private:
  std::vector<std::unique_ptr<InputPortBinding<InputsT>>> bindings_;
};

using OutputAlignmentCheck = std::function<TraceableAlignmentResult(
    const std::string&, const IProvenanceReader&)>;

template <typename ValueT>
class OutputBinding {
 public:
  virtual ~OutputBinding() = default;
  virtual NodePortDefinition Definition() const = 0;
  virtual const std::string& Anchor() const = 0;
  virtual bool Bind(const NodeInitContext&) = 0;
  virtual std::optional<NodeFailure> Validate(
      const ValueT&, const OutputAlignmentCheck&) const = 0;
  virtual bool ConflictsWithMember(const OutputBinding& other) const = 0;
  virtual void Publish(AlgContext&, ValueT&) const = 0;
  virtual std::unique_ptr<OutputBinding> Clone() const = 0;
};

template <typename ValueT, typename BatchT>
class TypedOutputBinding final : public OutputBinding<ValueT> {
 public:
  using Accessor = std::function<BatchT&(ValueT&)>;
  using ConstAccessor = std::function<const BatchT&(const ValueT&)>;
  TypedOutputBinding(std::string name, Accessor access, ConstAccessor read,
                     PortFlow flow, std::string anchor,
                     BatchT ValueT::*member = nullptr)
      : port_(std::move(name)),
        access_(std::move(access)),
        read_(std::move(read)),
        flow_(std::move(flow)),
        anchor_(std::move(anchor)),
        member_(member) {}
  NodePortDefinition Definition() const override {
    return {port_.LogicalName(),
            BlackboardTypeTraits<BatchT>::TypeName(),
            true,
            flow_.cardinality,
            flow_.provenance,
            flow_.lifetime,
            flow_.lifetime_config_field};
  }
  const std::string& Anchor() const override { return anchor_; }
  bool Bind(const NodeInitContext& init) override {
    auto result =
        detail::ResolvePortBinding(*init.plan, PortDirection::kOutput, port_);
    if (result.status == detail::PortBindingStatus::kUnbound)
      return init.Fail("Output port '" + port_.LogicalName() +
                       "' has no binding in plan");
    if (result.status == detail::PortBindingStatus::kTypeMismatch)
      return init.Fail("Output port type mismatch for '" + port_.LogicalName() +
                       "' (expected: " + port_.TypeId() +
                       ", bound: " + result.binding->type_id + ")");
    return true;
  }
  std::optional<NodeFailure> Validate(
      const ValueT& value, const OutputAlignmentCheck& check) const override {
    if (anchor_.empty()) return std::nullopt;
    TraceableBatchProvenanceReader<BatchT> reader(read_(value));
    auto result = check(anchor_, reader);
    if (result.error == TraceableAlignmentError::kCountMismatch)
      return NodeFailure{
          NodeErrorKind::kOutputCountMismatch,
          "Output port '" + port_.LogicalName() + "' count mismatch",
          node_error::author_node::kOutputCountMismatch};
    if (result.error == TraceableAlignmentError::kProvenanceMismatch)
      return NodeFailure{
          NodeErrorKind::kOutputProvenanceMismatch,
          "Output port '" + port_.LogicalName() + "' provenance mismatch",
          node_error::author_node::kOutputProvenanceMismatch};
    return std::nullopt;
  }
  bool ConflictsWithMember(const OutputBinding<ValueT>& other) const override {
    const auto* typed = dynamic_cast<const TypedOutputBinding*>(&other);
    return member_ && typed && member_ == typed->member_;
  }
  void Publish(AlgContext& context, ValueT& value) const override {
    port_.Set(context, std::move(access_(value)));
  }
  std::unique_ptr<OutputBinding<ValueT>> Clone() const override {
    return std::make_unique<TypedOutputBinding>(*this);
  }

 private:
  BoundOutput<BatchT> port_;
  Accessor access_;
  ConstAccessor read_;
  PortFlow flow_;
  std::string anchor_;
  BatchT ValueT::*member_;
};

template <typename ValueT>
class OutputBindingHolder {
 public:
  OutputBindingHolder(std::unique_ptr<OutputBinding<ValueT>> binding)  // NOLINT
      : binding_(std::move(binding)) {}
  OutputBindingHolder(const OutputBindingHolder& other)
      : binding_(other.binding_->Clone()) {}
  OutputBindingHolder(OutputBindingHolder&&) noexcept = default;
  std::unique_ptr<OutputBinding<ValueT>> Clone() const {
    return binding_->Clone();
  }

 private:
  std::unique_ptr<OutputBinding<ValueT>> binding_;
};

template <typename ValueT, typename BatchT>
OutputBindingHolder<ValueT> Produced(std::string name, BatchT ValueT::*member,
                                     PortFlow flow = {},
                                     std::string anchor = {}) {
  return OutputBindingHolder<ValueT>(
      std::make_unique<TypedOutputBinding<ValueT, BatchT>>(
          std::move(name), [member](ValueT& v) -> BatchT& { return v.*member; },
          [member](const ValueT& v) -> const BatchT& { return v.*member; },
          std::move(flow), std::move(anchor), member));
}

template <typename ValueT, typename BatchT>
OutputBindingHolder<ValueT> Produced(std::string name, BatchT ValueT::*member,
                                     std::string anchor) {
  if (anchor.empty())
    throw std::invalid_argument("Preserved output requires an anchor input");
  return Produced(std::move(name), member, PortFlow{}, std::move(anchor));
}

template <typename ValueT>
class OutputsOf {
 public:
  OutputsOf(std::initializer_list<OutputBindingHolder<ValueT>> bindings) {
    std::unordered_set<std::string> names;
    for (const auto& binding : bindings) {
      auto output = binding.Clone();
      const auto name = output->Definition().logical_name;
      if (!names.insert(name).second)
        throw std::invalid_argument("Duplicate output port");
      for (const auto& previous : bindings_)
        if (output->ConflictsWithMember(*previous))
          throw std::invalid_argument(
              "Output ports '" + previous->Definition().logical_name +
              "' and '" + name + "' bind the same result member");
      bindings_.push_back(std::move(output));
    }
  }
  OutputsOf(const OutputsOf& other) {
    for (const auto& binding : other.bindings_)
      bindings_.push_back(binding->Clone());
  }
  OutputsOf(OutputsOf&&) noexcept = default;
  OutputsOf& operator=(OutputsOf&&) noexcept = default;
  OutputsOf& operator=(const OutputsOf& other) {
    if (this != &other) {
      OutputsOf copy(other);
      *this = std::move(copy);
    }
    return *this;
  }
  std::vector<NodePortDefinition> ToPortDefinitions() const {
    std::vector<NodePortDefinition> definitions;
    for (const auto& binding : bindings_)
      definitions.push_back(binding->Definition());
    return definitions;
  }
  template <typename InputsT>
  void CheckAnchors(const InputsOf<InputsT>& inputs) const {
    for (const auto& binding : bindings_) {
      if (!binding->Anchor().empty() && !inputs.HasPort(binding->Anchor()))
        throw std::invalid_argument("PreservedOutput anchor port '" +
                                    binding->Anchor() +
                                    "' not found in declared inputs");
    }
  }
  bool BindPorts(const NodeInitContext& init) {
    for (auto& binding : bindings_)
      if (!binding->Bind(init)) return false;
    return true;
  }
  template <typename InputsT>
  std::optional<NodeFailure> Validate(const InputsT& inputs,
                                      const InputsOf<InputsT>& ports,
                                      const ValueT& value) const {
    OutputAlignmentCheck check = [&](const std::string& name,
                                     const IProvenanceReader& output) {
      const auto* port = ports.FindPort(name);
      if (!port)
        throw std::logic_error("Output anchor input is not declared: " + name);
      if (!port->HasBatch(inputs))
        return TraceableAlignmentResult{
            output.Size() == 0 ? TraceableAlignmentError::kNone
                               : TraceableAlignmentError::kCountMismatch,
            0};
      return port->ValidateAlignment(inputs, output);
    };
    for (const auto& binding : bindings_) {
      if (auto failure = binding->Validate(value, check)) return failure;
    }
    return std::nullopt;
  }
  void Publish(AlgContext& context, ValueT&& value) const {
    for (const auto& binding : bindings_) binding->Publish(context, value);
  }

 private:
  std::vector<std::unique_ptr<OutputBinding<ValueT>>> bindings_;
};

template <typename BatchT>
OutputsOf<BatchT> ProducedBatch(std::string name, PortFlow flow = {},
                                std::string anchor = {}) {
  return OutputsOf<BatchT>({OutputBindingHolder<BatchT>(
      std::make_unique<TypedOutputBinding<BatchT, BatchT>>(
          std::move(name), [](BatchT& v) -> BatchT& { return v; },
          [](const BatchT& v) -> const BatchT& { return v; }, std::move(flow),
          std::move(anchor)))});
}

template <typename BatchT>
OutputsOf<BatchT> PreservedOutput(std::string name, std::string anchor,
                                  PortFlow flow = {}) {
  if (anchor.empty())
    throw std::invalid_argument("Preserved output requires an anchor input");
  return ProducedBatch<BatchT>(std::move(name), std::move(flow),
                               std::move(anchor));
}

template <typename ModelsT>
class ModelSlotBinding {
 public:
  virtual ~ModelSlotBinding() = default;
  virtual const std::string& SlotName() const = 0;
  virtual const std::string& ConfigField() const = 0;
  virtual const std::string& Capability() const = 0;
  virtual ConfigFieldDefinition ToConfigField() const = 0;
  virtual bool Bind(const NodeInitContext& init_ctx,
                    SessionContext& session_ctx,
                    const nlohmann::json& normalized_config, ModelsT* models,
                    std::string* err) = 0;
  virtual std::unique_ptr<ModelSlotBinding<ModelsT>> Clone() const = 0;
};

template <typename ModelsT, typename CallT>
class TypedModelSlotBinding final : public ModelSlotBinding<ModelsT> {
 public:
  TypedModelSlotBinding(std::string slot, std::string field,
                        CallT ModelsT::*member)
      : slot_(std::move(slot)), field_(std::move(field)), member_(member) {}
  const std::string& SlotName() const override { return slot_; }
  const std::string& ConfigField() const override { return field_; }
  const std::string& Capability() const override {
    static const std::string capability =
        ModelCapabilityTraits<typename CallT::ModelType>::Capability();
    return capability;
  }
  ConfigFieldDefinition ToConfigField() const override {
    ConfigFieldDefinition field;
    field.name = field_;
    field.kind = ConfigValueKind::kString;
    field.required = true;
    field.semantic =
        "引用 models[].model_id；所选模型的类别必须是 " + Capability();
    return field;
  }
  bool Bind(const NodeInitContext& init, SessionContext& session,
            const nlohmann::json&, ModelsT* models,
            std::string* error) override {
    std::string model_id;
    if (!ResolveBoundModelId(*init.plan, slot_, Capability(), &model_id, error))
      return false;
    auto model =
        session.GetModelManager().GetModel<typename CallT::ModelType>(model_id);
    if (!model) {
      if (error)
        *error = Capability() + " model '" + model_id +
                 "' is unavailable or incompatible";
      return false;
    }
    models->*member_ = CallT(std::move(model), slot_, model_id);
    return true;
  }
  std::unique_ptr<ModelSlotBinding<ModelsT>> Clone() const override {
    return std::make_unique<TypedModelSlotBinding>(*this);
  }

 private:
  std::string slot_;
  std::string field_;
  CallT ModelsT::*member_;
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

template <typename ModelsT, typename CallT>
inline ModelSlotBindingHolder<ModelsT> Model(std::string slot,
                                             std::string field,
                                             CallT ModelsT::*member) {
  return ModelSlotBindingHolder<ModelsT>(
      std::make_unique<TypedModelSlotBinding<ModelsT, CallT>>(
          std::move(slot), std::move(field), member));
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
      fields.push_back(b->ToConfigField());
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
  static const std::vector<std::unique_ptr<ModelSlotBinding<NoModels>>>&
  Bindings() noexcept {
    static const std::vector<std::unique_ptr<ModelSlotBinding<NoModels>>> empty;
    return empty;
  }
};

template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
class NodeSpec {
 public:
  using InputsType = InputsT;
  using OutputBatch = OutputBatchT;
  using ParametersType = ParamsT;
  using ModelsType = ModelsT;
  using RunFunctionType = RunFnT;

  using RunSignature = detail::RunSignature<RunFnT, InputsT, ParamsT, ModelsT>;
  using RunResult = std::decay_t<typename RunSignature::Result>;
  static constexpr bool kRunResultValid =
      IsNodeResult<RunResult>::value &&
      std::is_convertible_v<typename IsNodeResult<RunResult>::ValueType,
                            OutputBatchT>;
  static constexpr bool kRunSignatureValid =
      RunSignature::kCallable && kRunResultValid;
  static_assert(RunSignature::kCallable,
                "Node Run must be callable as NodeResult<OutputBatch> "
                "Run(const Inputs&[, const Params&][, const Models&][, const "
                "SessionResources&]); write Params only with Parameters<...> "
                "and Models only with ModelsOf<...>. See "
                "doc/dev_guide/custom_node_concepts.md");
  static_assert(!RunSignature::kCallable || kRunResultValid,
                "Node Run must return NodeResult<OutputBatch> "
                "(NodeResult<Outputs> for multiple outputs)");

  NodeSpec(InputsOf<InputsT> inputs, OutputsOf<OutputBatchT> output,
           Parameters<ParamsT> params, ModelsOf<ModelsT> models, RunFnT fn)
      : inputs_(std::move(inputs)),
        output_(std::move(output)),
        params_(std::move(params)),
        models_(std::move(models)),
        fn_(std::move(fn)) {
    output_.CheckAnchors(inputs_);
  }

  NodeSpec& Description(std::string desc) & {
    description_ = std::move(desc);
    return *this;
  }
  NodeSpec Description(std::string desc) && {
    description_ = std::move(desc);
    return std::move(*this);
  }

  NodeSpec& Category(std::string cat) & {
    category_ = std::move(cat);
    return *this;
  }
  NodeSpec Category(std::string cat) && {
    category_ = std::move(cat);
    return std::move(*this);
  }

  NodeSpec& ParallelSafe(bool safe) & {
    parallel_safe_ = safe;
    return *this;
  }
  NodeSpec ParallelSafe(bool safe) && {
    parallel_safe_ = safe;
    return std::move(*this);
  }

  NodeSpec WithControls(std::vector<FieldControlCommand> commands) && {
    static_assert(std::is_copy_constructible_v<ParamsT>,
                  "WithControls requires copy-constructible ParametersType");
    ValidateControlCommands(commands, params_, &models_);
    control_commands_ = std::move(commands);
    return std::move(*this);
  }

  NodeSpec WithControls(
      std::initializer_list<FieldControlCommand> commands) && {
    return std::move(*this).WithControls(
        std::vector<FieldControlCommand>(commands));
  }

  NodeSpec& PortConstraints(std::vector<PortGroupConstraint> constraints) & {
    port_constraints_ = std::move(constraints);
    return *this;
  }
  NodeSpec PortConstraints(std::vector<PortGroupConstraint> constraints) && {
    port_constraints_ = std::move(constraints);
    return std::move(*this);
  }
  bool HasControls() const noexcept { return !control_commands_.empty(); }

  const std::vector<FieldControlCommand>& ControlCommands() const noexcept {
    return control_commands_;
  }

  InputsOf<InputsT>& Inputs() noexcept { return inputs_; }
  const InputsOf<InputsT>& Inputs() const noexcept { return inputs_; }
  OutputsOf<OutputBatchT>& Output() noexcept { return output_; }
  const OutputsOf<OutputBatchT>& Output() const noexcept { return output_; }
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
    def.inputs = inputs_.ToPortDefinitions();
    def.outputs = output_.ToPortDefinitions();
    def.port_constraints = port_constraints_;

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
                              std::string* err, std::string* field_path) {
      return params.ValidateWithBindings(cfg, conn, err, field_path);
    };
    for (const auto& cmd : control_commands_) {
      def.control_commands.push_back(cmd.ToCommandDefinition(params_));
    }
    return def;
  }

 private:
  InputsOf<InputsT> inputs_;
  OutputsOf<OutputBatchT> output_;
  Parameters<ParamsT> params_;
  ModelsOf<ModelsT> models_;
  RunFnT fn_;
  std::vector<FieldControlCommand> control_commands_;
  std::vector<PortGroupConstraint> port_constraints_;
  std::string category_ = "custom";
  std::string description_;
  bool parallel_safe_ = false;
};

// Parameters 与 ModelsOf 可省略；省略的部分也不出现在 Run 的参数中。
template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
inline auto MakeNodeSpec(InputsOf<InputsT> inputs,
                         OutputsOf<OutputBatchT> output,
                         Parameters<ParamsT> params, ModelsOf<ModelsT> models,
                         RunFnT fn) {
  return NodeSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>(
      std::move(inputs), std::move(output), std::move(params),
      std::move(models), std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename ModelsT,
          typename RunFnT>
inline auto MakeNodeSpec(InputsOf<InputsT> inputs,
                         OutputsOf<OutputBatchT> output,
                         ModelsOf<ModelsT> models, RunFnT fn) {
  return NodeSpec<InputsT, OutputBatchT, NoParameters, ModelsT, RunFnT>(
      std::move(inputs), std::move(output), Parameters<NoParameters>{},
      std::move(models), std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename RunFnT>
inline auto MakeNodeSpec(InputsOf<InputsT> inputs,
                         OutputsOf<OutputBatchT> output,
                         Parameters<ParamsT> params, RunFnT fn) {
  return NodeSpec<InputsT, OutputBatchT, ParamsT, NoModels, RunFnT>(
      std::move(inputs), std::move(output), std::move(params),
      ModelsOf<NoModels>{}, std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename RunFnT>
inline auto MakeNodeSpec(InputsOf<InputsT> inputs,
                         OutputsOf<OutputBatchT> output, RunFnT fn) {
  return NodeSpec<InputsT, OutputBatchT, NoParameters, NoModels, RunFnT>(
      std::move(inputs), std::move(output), Parameters<NoParameters>{},
      ModelsOf<NoModels>{}, std::move(fn));
}

// ---------------------------------------------------------------------------
// AuthorNode：由 NodeSpec 生成 NodeBase 运行时
// ---------------------------------------------------------------------------

template <typename SpecT>
class AuthorNode;

template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
class AuthorNode<NodeSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>>
    : public NodeBase {
 public:
  using SpecType = NodeSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>;

  AuthorNode(std::string node_name, SpecType spec)
      : NodeBase(std::move(node_name)), spec_(std::move(spec)) {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& session_ctx) override {
    if (!spec_.Inputs().BindPorts(init_ctx)) {
      return false;
    }

    if (!spec_.Output().BindPorts(init_ctx)) return false;
    resources_ = SessionResources(session_ctx);

    const auto& normalized = config;

    binding_facts_ = MakeBindingFacts(init_ctx);

    std::string err;
    auto parsed = spec_.ParametersSpec().ParseNormalized(normalized,
                                                         binding_facts_, &err);
    if (!parsed) {
      return init_ctx.Fail(err.empty() ? "Invalid node configuration" : err);
    }
    if (spec_.HasControls()) {
      snapshot_.Initialize(std::move(*parsed));
    } else {
      parameters_ = std::move(*parsed);
    }

    if (!spec_.Models().BindModels(init_ctx, session_ctx, normalized, &models_,
                                   &err)) {
      return init_ctx.Fail(err.empty() ? "Failed to bind models" : err);
    }

    return true;
  }

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (!spec_.HasControls()) {
      return NodeControlResult::Unsupported();
    }
    if constexpr (std::is_copy_constructible_v<
                      typename SpecType::ParametersType>) {
      for (const auto& command : spec_.ControlCommands()) {
        if (command.Id() == cmd) {
          return command.Execute(spec_.ParametersSpec(), json_param,
                                 binding_facts_, snapshot_);
        }
      }
    }
    return NodeControlResult::Unsupported();
  }

  int ProcessNode(AlgContext& req_ctx) override {
    if constexpr (SpecType::kRunSignatureValid) {
      InputsT inputs{};
      std::string err;
      if (!spec_.Inputs().PopulateInputs(req_ctx, &inputs, &err)) {
        return this->Fail(req_ctx, node_error::author_node::kMissingInput,
                          err.empty() ? "Failed to populate inputs" : err);
      }

      std::shared_ptr<const typename SpecType::ParametersType> snapshot_guard;
      const typename SpecType::ParametersType* params_ptr = nullptr;
      if (spec_.HasControls()) {
        snapshot_guard = snapshot_.Read();
        if (!snapshot_guard) {
          return this->Fail(req_ctx, node_error::author_node::kInternalError,
                            this->Name() + ": snapshot not initialized");
        }
        params_ptr = snapshot_guard.get();
      } else {
        params_ptr = &parameters_;
      }

      auto res = detail::InvokeRun(spec_.Function(), inputs, *params_ptr,
                                   models_, resources_);
      if (!res.ok()) {
        auto failure = std::move(res).ExtractFailure();
        int code = failure.cause_code != 0
                       ? failure.cause_code
                       : node_error::author_node::kBusinessError;
        return this->Fail(
            req_ctx, code,
            failure.FormatDiagnostic(this->Name() + " process failed"));
      }

      OutputBatchT output = std::move(res).value();

      if (auto failure =
              spec_.Output().Validate(inputs, spec_.Inputs(), output)) {
        return this->Fail(req_ctx, failure->cause_code,
                          failure->FormatDiagnostic());
      }
      spec_.Output().Publish(req_ctx, std::move(output));
      return 0;
    } else {
      // 合法程序中不可达；避免引发连锁模板错误。
      return this->Fail(req_ctx, node_error::author_node::kInternalError,
                        "Invalid Node Run signature");
    }
  }

 private:
  SpecType spec_;
  SessionResources resources_;
  typename SpecType::ParametersType parameters_{};
  typename SpecType::ModelsType models_{};
  ConfigurationSnapshot<typename SpecType::ParametersType> snapshot_;
  BindingFacts binding_facts_;
};

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
