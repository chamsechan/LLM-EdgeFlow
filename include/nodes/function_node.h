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
#include "engine/model_capability_traits.h"
#include "nodes/configuration_snapshot.h"
#include "nodes/control_authoring.h"
#include "nodes/model_binding.h"
#include "nodes/model_calls.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"
#include "nodes/parameter_binding.h"
#include "nodes/session_resources.h"
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

template <>
struct BatchItemTraits<ImageRefBatch>
    : BatchItemTraits<std::vector<TraceableItem<std::string>>> {};

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
                 const ModelsT& models, const SessionResources& resources) {
  if constexpr (std::is_member_function_pointer_v<Fn>) {
    using ClassT = typename MemberFunctionTraits<Fn>::ClassType;
    ClassT logic{};
    if constexpr (std::is_invocable_v<Fn, ClassT, const InputsT&,
                                      const ParamsT&, const ModelsT&,
                                      const SessionResources&>) {
      return (logic.*fn)(inputs, params, models, resources);
    } else if constexpr (std::is_invocable_v<Fn, ClassT, const InputsT&,
                                             const ParamsT&, const ModelsT&>) {
      return (logic.*fn)(inputs, params, models);
    } else {
      return (logic.*fn)(inputs, params);
    }
  } else {
    if constexpr (std::is_invocable_v<Fn, const InputsT&, const ParamsT&,
                                      const ModelsT&,
                                      const SessionResources&>) {
      return fn(inputs, params, models, resources);
    } else if constexpr (std::is_invocable_v<Fn, const InputsT&, const ParamsT&,
                                             const ModelsT&>) {
      return fn(inputs, params, models);
    } else {
      return fn(inputs, params);
    }
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

  MapSpec WithControls(std::vector<FieldControlCommand> commands) && {
    static_assert(std::is_copy_constructible_v<ParamsT>,
                  "WithControls requires copy-constructible ParametersType");
    ValidateControlCommands(commands, params_);
    control_commands_ = std::move(commands);
    return std::move(*this);
  }

  MapSpec WithControls(std::initializer_list<FieldControlCommand> commands) && {
    return std::move(*this).WithControls(
        std::vector<FieldControlCommand>(commands));
  }

  bool HasControls() const noexcept { return !control_commands_.empty(); }

  const std::vector<FieldControlCommand>& ControlCommands() const noexcept {
    return control_commands_;
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
    for (const auto& cmd : control_commands_) {
      def.control_commands.push_back(cmd.ToCommandDefinition(params_));
    }
    return def;
  }

 private:
  Input<InputBatchT> in_;
  Output<OutputBatchT> out_;
  Parameters<ParamsT> params_;
  MapFnT fn_;
  std::vector<FieldControlCommand> control_commands_;
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

// Flow metadata describes the existing port contract; it does not transform
// data.
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
        return init_ctx.Fail("Input port type mismatch for '" + name_ + "'");
      }
    } else {
      if (required_) {
        return init_ctx.Fail("Required input port '" + name_ +
                             "' is unbound in plan");
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

// Optional connection whose absent request value is handled by the algorithm
// (e.g. template missing-variable policy or conditionally used context).
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
                       "'");
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
                        CallT ModelsT::*member, std::string default_id,
                        std::string description)
      : slot_(std::move(slot)),
        field_(std::move(field)),
        member_(member),
        default_id_(std::move(default_id)),
        description_(std::move(description)) {}
  const std::string& SlotName() const override { return slot_; }
  const std::string& ConfigField() const override { return field_; }
  const std::string& Capability() const override {
    static const std::string capability =
        ModelCapabilityTraits<typename CallT::ModelType>::Capability();
    return capability;
  }
  ConfigFieldDefinition ToConfigField() const override {
    return {
        field_,
        ConfigValueKind::kString,
        default_id_.empty(),
        default_id_.empty() ? nlohmann::json() : nlohmann::json(default_id_),
        std::nullopt,
        std::nullopt,
        {},
        description_.empty() ? "Bound model ID for slot '" + slot_ + "'"
                             : description_};
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
  std::string default_id_;
  std::string description_;
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
                                             CallT ModelsT::*member,
                                             std::string default_id = {},
                                             std::string description = {}) {
  return ModelSlotBindingHolder<ModelsT>(
      std::make_unique<TypedModelSlotBinding<ModelsT, CallT>>(
          std::move(slot), std::move(field), member, std::move(default_id),
          std::move(description)));
}

template <typename ModelsT>
inline ModelSlotBindingHolder<ModelsT> Llm(std::string slot, std::string field,
                                           LlmCall ModelsT::*member) {
  return Model(std::move(slot), std::move(field), member);
}
template <typename ModelsT>
inline ModelSlotBindingHolder<ModelsT> Embedding(
    std::string slot, std::string field, EmbeddingCall ModelsT::*member) {
  return Model(std::move(slot), std::move(field), member);
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
class BatchSpec {
 public:
  using InputsType = InputsT;
  using OutputBatch = OutputBatchT;
  using ParametersType = ParamsT;
  using ModelsType = ModelsT;
  using RunFunctionType = RunFnT;

  BatchSpec(InputsOf<InputsT> inputs, OutputsOf<OutputBatchT> output,
            Parameters<ParamsT> params, ModelsOf<ModelsT> models, RunFnT fn)
      : inputs_(std::move(inputs)),
        output_(std::move(output)),
        params_(std::move(params)),
        models_(std::move(models)),
        fn_(std::move(fn)) {
    output_.CheckAnchors(inputs_);
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

  BatchSpec WithControls(std::vector<FieldControlCommand> commands) && {
    static_assert(std::is_copy_constructible_v<ParamsT>,
                  "WithControls requires copy-constructible ParametersType");
    ValidateControlCommands(commands, params_, &models_);
    for (const auto& command : commands) {
      for (const auto& custom : custom_controls_) {
        if (command.Id() == custom.definition.cmd_id)
          throw std::invalid_argument("Duplicate Control command");
      }
    }
    control_commands_ = std::move(commands);
    return std::move(*this);
  }

  BatchSpec WithControls(
      std::initializer_list<FieldControlCommand> commands) && {
    return std::move(*this).WithControls(
        std::vector<FieldControlCommand>(commands));
  }

  BatchSpec& PortConstraints(std::vector<PortGroupConstraint> constraints) & {
    port_constraints_ = std::move(constraints);
    return *this;
  }
  BatchSpec PortConstraints(std::vector<PortGroupConstraint> constraints) && {
    port_constraints_ = std::move(constraints);
    return std::move(*this);
  }
  using ControlUpdater = std::function<NodeResult<ParamsT>(
      const ParamsT&, const nlohmann::json&, const BindingFacts&)>;
  BatchSpec WithControl(ControlCommandDefinition definition,
                        ControlUpdater update) && {
    for (const auto& cmd : control_commands_) {
      if (cmd.Id() == definition.cmd_id)
        throw std::invalid_argument("Duplicate Control command");
    }
    for (const auto& cmd : custom_controls_) {
      if (cmd.definition.cmd_id == definition.cmd_id)
        throw std::invalid_argument("Duplicate Control command");
    }
    custom_controls_.push_back({std::move(definition), std::move(update)});
    return std::move(*this);
  }
  struct CustomControl {
    ControlCommandDefinition definition;
    ControlUpdater update;
  };
  const std::vector<CustomControl>& CustomControls() const {
    return custom_controls_;
  }

  bool HasControls() const noexcept {
    return !control_commands_.empty() || !custom_controls_.empty();
  }

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
    def.biz_names = biz_names_;
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
                              std::string* err) {
      return params.ValidateWithBindings(cfg, conn, err);
    };
    for (const auto& cmd : control_commands_) {
      def.control_commands.push_back(cmd.ToCommandDefinition(params_));
    }
    for (const auto& cmd : custom_controls_)
      def.control_commands.push_back(cmd.definition);
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
  std::vector<CustomControl> custom_controls_;
  std::string category_ = "custom";
  std::string description_;
  bool parallel_safe_ = false;
  std::vector<std::string> biz_names_;
};

// Batch overloads
template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          OutputsOf<OutputBatchT> output,
                          Parameters<ParamsT> params, ModelsOf<ModelsT> models,
                          RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>(
      std::move(inputs), std::move(output), std::move(params),
      std::move(models), std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename ModelsT,
          typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          OutputsOf<OutputBatchT> output,
                          ModelsOf<ModelsT> models, RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, NoParameters, ModelsT, RunFnT>(
      std::move(inputs), std::move(output), Parameters<NoParameters>{},
      std::move(models), std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          OutputsOf<OutputBatchT> output,
                          Parameters<ParamsT> params, RunFnT fn) {
  return BatchSpec<InputsT, OutputBatchT, ParamsT, NoModels, RunFnT>(
      std::move(inputs), std::move(output), std::move(params),
      ModelsOf<NoModels>{}, std::move(fn));
}

template <typename InputsT, typename OutputBatchT, typename RunFnT>
inline auto MakeBatchSpec(InputsOf<InputsT> inputs,
                          OutputsOf<OutputBatchT> output, RunFnT fn) {
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
    const auto in_result = detail::ResolvePortBinding(
        *init_ctx.plan, PortDirection::kInput, in_port_);
    if (in_result.status == detail::PortBindingStatus::kUnbound) {
      return init_ctx.Fail("Required input port '" + spec_.InputName() +
                           "' has no binding in plan");
    }
    if (in_result.status == detail::PortBindingStatus::kTypeMismatch) {
      return init_ctx.Fail("Input port type mismatch for '" +
                           spec_.InputName() +
                           "' (expected: " + in_port_.TypeId() +
                           ", bound: " + in_result.binding->type_id + ")");
    }

    const auto out_result = detail::ResolvePortBinding(
        *init_ctx.plan, PortDirection::kOutput, out_port_);
    if (out_result.status == detail::PortBindingStatus::kUnbound) {
      return init_ctx.Fail("Output port '" + spec_.OutputName() +
                           "' has no binding in plan");
    }
    if (out_result.status == detail::PortBindingStatus::kTypeMismatch) {
      return init_ctx.Fail("Output port type mismatch for '" +
                           spec_.OutputName() +
                           "' (expected: " + out_port_.TypeId() +
                           ", bound: " + out_result.binding->type_id + ")");
    }

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
    const auto& params = *params_ptr;

    OutputBatch outputs;
    outputs.reserve(inputs->size());

    for (const auto& item : *inputs) {
      if constexpr (SpecType::kReturnsNodeResult) {
        auto res = detail::InvokeMapItem(spec_.Function(), item.data, params);
        if (!res.ok()) {
          auto failure = std::move(res).ExtractFailure();
          if (!failure.batch_detail.has_value()) {
            failure.batch_detail = BatchFailureDetail{
                this->Name(), BatchFailureReason::kCallbackFailed,
                TraceableItemKey{item.req_id, item.sub_id}};
          }
          int code = failure.cause_code != 0
                         ? failure.cause_code
                         : node_error::author_node::kBusinessError;
          return this->Fail(
              req_ctx, code,
              failure.FormatDiagnostic(this->Name() + " map function failed"));
        }
        outputs.emplace_back(item.req_id, item.sub_id, std::move(res).value());
      } else {
        outputs.emplace_back(
            item.req_id, item.sub_id,
            detail::InvokeMapItem(spec_.Function(), item.data, params));
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
  ConfigurationSnapshot<typename SpecType::ParametersType> snapshot_;
  BindingFacts binding_facts_;
};

// BatchSpec Specialization
template <typename InputsT, typename OutputBatchT, typename ParamsT,
          typename ModelsT, typename RunFnT>
class AuthorNode<BatchSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>>
    : public NodeBase {
 public:
  using SpecType = BatchSpec<InputsT, OutputBatchT, ParamsT, ModelsT, RunFnT>;

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
    for (const auto& command : spec_.CustomControls()) {
      if (command.definition.cmd_id != cmd) continue;
      nlohmann::json payload;
      std::string error;
      if (!ParseControlPayload(json_param, command.definition.payload_schema,
                               &payload, &error)) {
        return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                         error);
      }
      return snapshot_.Update([&](const auto& current) {
        return command.update(current, payload, binding_facts_);
      });
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

    auto res = detail::InvokeBatch(spec_.Function(), inputs, *params_ptr,
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
  }

 private:
  SpecType spec_;
  SessionResources resources_;
  typename SpecType::ParametersType parameters_{};
  typename SpecType::ModelsType models_{};
  ConfigurationSnapshot<typename SpecType::ParametersType> snapshot_;
  BindingFacts binding_facts_;
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
