/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/tools/multihost_hlo_runner/functional_hlo_runner.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/btree_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"
#include "xla/client/executable_build_options.h"
#include "xla/hlo/builder/xla_computation.h"
#include "xla/hlo/ir/hlo_input_output_alias_config.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_sharding.h"
#include "xla/hlo/parser/hlo_parser.h"
#include "xla/hlo/pass/hlo_pass_pipeline.h"
#include "xla/hlo/transforms/while_loop_trip_count_annotator.h"
#include "xla/hlo/translate/hlo_to_mhlo/translate.h"
#include "xla/hlo/translate/stablehlo.h"
#include "xla/layout.h"
#include "xla/layout_util.h"
#include "xla/literal.h"
#include "xla/literal_util.h"
#include "xla/pjrt/distributed/key_value_store_interface.h"
#include "xla/pjrt/host_memory_spaces.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/pjrt/pjrt_compiler.h"
#include "xla/pjrt/pjrt_executable.h"
#include "xla/pjrt/pjrt_future.h"
#include "xla/pjrt/pjrt_layout.h"
#include "xla/primitive_util.h"
#include "xla/runtime/large_hlo_snapshot_serialization/serialization.h"
#include "xla/service/computation_layout.h"
#include "xla/service/computation_placer.h"
#include "xla/service/hlo.pb.h"
#include "xla/service/hlo_module_config.h"
#include "xla/service/hlo_module_util.h"
#include "xla/service/slow_operation_alarm.h"
#include "xla/shape.h"
#include "xla/shape_layout.h"
#include "xla/shape_util.h"
#include "xla/status_macros.h"
#include "xla/tests/test_utils.h"
#include "xla/tools/hlo_control_flow_flattening.h"
#include "xla/tsl/platform/env.h"
#include "xla/tsl/platform/errors.h"
#include "xla/tsl/platform/file_system.h"
#include "xla/tsl/platform/file_system_helper.h"
#include "xla/tsl/platform/status.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/tsl/util/fixed_option_set_flag.h"
#include "xla/util.h"
#include "xla/xla.pb.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/protobuf.h"
#include "tsl/profiler/lib/profiler_session.h"
#include "tsl/profiler/protobuf/profiler_options.pb.h"
#include "tsl/profiler/protobuf/xplane.pb.h"

namespace xla {
namespace FunctionalHloRunner {
namespace {
using HloModuleAndArguments = ::xla::FunctionalHloRunner::HloModuleAndArguments;

absl::Span<PjRtDevice* const> GetLocalDevices(const PjRtClient& client) {
  return client.addressable_devices();
}

// Argument buffers are created on device at the first time an HLO module
// is executed. We reuse argument buffers in the following repeated
// executions whenever possible. We take the following strategy to
// maximally reuse on-device argument buffers which compiles and executes
// the HLO module differently depending on the number of parameters and the
// shape of the parameters of the HLO module. We have the following 3 cases.
// 1. The number of parameters is 1 and it has a shape of tuple of arrays.
// 2. The number of parameters is 1 or many and they are all arrays.
// 3. The rest: this should be rare and we don't expect this to happen with
// JAX.
//
// Case 1: the HLO module is compiled with
// CompileOptions::parameter_is_tupled_arguments = true
// and the HLO module is executed with
// ExecuteOptions::arguments_are_tupled = false.
// This enables PjRtClient::Execute to assemble the tupled arguments from
// a flat list of buffers.
// Additionally, we set ExecuteOptions::untuple_result = true if the module's
// output is a tuple. Thus we can use the aliased output buffer as input
// arguments and reuse the non-aliased argument buffers. In this mode, users may
// provide the argument literals as a list of tuples (for the convenience of
// future use cases) or a tuple literal (to support existing use cases).
//
// Case 2: the HLO module is compiled with
// CompileOptions::parameter_is_tupled_arguments = false
// and the HLO module is executed with
// ExecuteOptions::arguments_are_tupled = false.
// Same as above, we set ExecuteOptions::untuple_result = true if the module's
// output is a tuple. This allows us to reuse on-device buffers in the same way
// as case 1.
//
// Case 3: the HLO module is compiled with
// CompileOptions::parameter_is_tupled_arguments = false
// and the HLO module is executed with
// ExecuteOptions::arguments_are_tupled = false.
// We will create new on-device buffers for each repeated execution.
//
// Irrespective of the above, if the output is a tuple with leaves mixing host
// and device memory spaces, we set ExecuteOptions::untuple_result = true.
// Otherwise PJRT cannot correctly represent these tuples, because a PjRtBuffer
// can only belong to one memory space. By "untupling", PJRT assigns a separate
// PjRtBuffer to each leaf.

enum class ParameterType {
  kOneTupleOfArrays = 0,
  kOneListOfArrays = 1,
  kOther = 2
};

ParameterType GetParameterType(const HloModule& module) {
  int num_parameters = module.entry_computation()->num_parameters();
  if (num_parameters == 1) {
    const Shape& shape =
        module.entry_computation()->parameter_instruction(0)->shape();
    if (shape.IsTuple()) {
      bool is_tuple_of_arrays = absl::c_all_of(
          shape.tuple_shapes(),
          [](const Shape& subshape) { return subshape.IsArray(); });
      if (is_tuple_of_arrays) {
        return ParameterType::kOneTupleOfArrays;
      }
      return ParameterType::kOther;
    }
  }
  bool is_list_of_arrays =
      absl::c_all_of(module.entry_computation()->parameter_instructions(),
                     [](const HloInstruction* parameter) {
                       return parameter->shape().IsArray();
                     });
  return is_list_of_arrays ? ParameterType::kOneListOfArrays
                           : ParameterType::kOther;
}

template <typename ElementType>
void PopulateWithSameValue(Literal* literal, ElementType val) {
  for (ElementType& element : literal->data<ElementType>()) {
    element = static_cast<ElementType>(val);
  }
}

absl::StatusOr<Literal> MakeFakeLiteralWithSameValue(const Shape& shape,
                                                     int value) {
  if (shape.IsArray()) {
    Shape new_shape = shape;
    new_shape.mutable_layout()->clear_tiles();
    return primitive_util::PrimitiveTypeSwitch<absl::StatusOr<Literal>>(
        [&](auto type) -> absl::StatusOr<Literal> {
          if constexpr (primitive_util::IsArrayType(type)) {
            using NativeT = primitive_util::NativeTypeOf<type>;

            Literal literal(new_shape);
            PopulateWithSameValue(
                &literal,
                static_cast<NativeT>(type == PRED ? (value % 2) == 0 : value));
            for (int i = 0; i < shape.dimensions().size(); i++) {
              if (shape.is_dynamic_dimension(i)) {
                // TODO(b/378917570): We might need to set the dynamic size to
                // the actual bound i.e., shape.dimensions(i) when HybridSim
                // supports SparseCore.
                literal.SetDynamicSize(i, 0);
              }
            }
            return literal;
          }
          return Unimplemented(
              "Unsupported type for fake literal generation: %s",
              ShapeUtil::HumanString(shape));
        },
        new_shape.element_type());
  } else if (shape.IsTuple()) {
    std::vector<Literal> subliterals;
    for (const Shape& subshape : shape.tuple_shapes()) {
      TF_ASSIGN_OR_RETURN(Literal subliteral,
                          MakeFakeLiteralWithSameValue(subshape, value));
      subliterals.push_back(std::move(subliteral));
    }
    return LiteralUtil::MakeTupleOwned(std::move(subliterals));
  }
  return InvalidArgument("Unsupported type for fake literal generation: %s",
                         ShapeUtil::HumanString(shape));
}

absl::StatusOr<HloModuleAndArguments> ReadModuleFromSnapshotBinaryProtoFile(
    absl::string_view hlo_file) {
  HloSnapshot proto;
  HloModuleAndArguments hlo_module_and_arguments;
  TF_RETURN_IF_ERROR(
      tsl::ReadBinaryProto(tsl::Env::Default(), std::string(hlo_file), &proto));
  hlo_module_and_arguments.arguments.emplace_back();
  hlo_module_and_arguments.arguments.front().resize(proto.arguments_size());
  for (int i = 0; i < proto.arguments_size(); i++) {
    TF_ASSIGN_OR_RETURN(hlo_module_and_arguments.arguments.front()[i],
                        Literal::CreateFromProto(proto.arguments()[i]));
  }
  TF_ASSIGN_OR_RETURN(hlo_module_and_arguments.hlo_module,
                      CreateModuleFromProto(proto.hlo().hlo_module()));
  return hlo_module_and_arguments;
}

absl::StatusOr<HloModuleAndArguments>
ReadModuleFromUnoptimizedSnapshotBinaryProtoFile(absl::string_view hlo_file) {
  HloModuleAndArguments hlo_module_and_arguments;
  tsl::Env* env = tsl::Env::Default();

  std::unique_ptr<tsl::RandomAccessFile> file;
  TF_RETURN_IF_ERROR(env->NewRandomAccessFile(std::string(hlo_file), &file));

  tsl::RandomAccessFileCopyingInputStream input_stream(file.get());
  tsl::protobuf::io::CopyingInputStreamAdaptor adaptor(&input_stream);

  TF_ASSIGN_OR_RETURN(HloUnoptimizedSnapshot proto,
                      DeserializeHloUnoptimizedSnapshot(&adaptor));

  TF_ASSIGN_OR_RETURN(hlo_module_and_arguments.hlo_module,
                      CreateModuleFromProto(proto.hlo_module()));

  for (const auto& arguments : proto.partitions()) {
    hlo_module_and_arguments.arguments.emplace_back();
    hlo_module_and_arguments.arguments.back().reserve(
        arguments.arguments_size());
    for (const auto& argument : arguments.arguments()) {
      TF_ASSIGN_OR_RETURN(
          hlo_module_and_arguments.arguments.back().emplace_back(),
          Literal::CreateFromProto(argument));
    }
  }
  return hlo_module_and_arguments;
}

absl::StatusOr<HloModuleAndArguments>
ReadModuleFromUnoptimizedSnapshotTextProtoFile(absl::string_view hlo_file) {
  HloUnoptimizedSnapshot proto;
  HloModuleAndArguments hlo_module_and_arguments;
  TF_RETURN_IF_ERROR(
      tsl::ReadTextProto(tsl::Env::Default(), std::string(hlo_file), &proto));
  TF_ASSIGN_OR_RETURN(hlo_module_and_arguments.hlo_module,
                      CreateModuleFromProto(proto.hlo_module()));

  for (const auto& arguments : proto.partitions()) {
    hlo_module_and_arguments.arguments.emplace_back();
    hlo_module_and_arguments.arguments.back().reserve(
        arguments.arguments_size());
    for (const auto& argument : arguments.arguments()) {
      TF_ASSIGN_OR_RETURN(
          hlo_module_and_arguments.arguments.back().emplace_back(),
          Literal::CreateFromProto(argument));
    }
  }
  return hlo_module_and_arguments;
}

ReplicasAndPartitions GetReplicasAndPartitionsInternal(
    const std::optional<ExecutionOptions>& execution_options, int device_count,
    const std::optional<int>& num_replicas,
    const std::optional<int>& num_partitions, int num_slices = 1) {
  if (num_replicas.has_value() && num_partitions.has_value()) {
    return ReplicasAndPartitions{num_replicas.value(), num_partitions.value()};
  }
  if (execution_options.has_value()) {
    return ReplicasAndPartitions{execution_options->num_replicas(),
                                 execution_options->num_partitions()};
  }
  if (num_replicas.has_value()) {
    return ReplicasAndPartitions{
        num_replicas.value(), device_count * num_slices / num_replicas.value()};
  }
  if (num_partitions.has_value()) {
    return ReplicasAndPartitions{
        device_count * num_slices / num_partitions.value(),
        num_partitions.value()};
  }
  return ReplicasAndPartitions{device_count * num_slices, 1};
}

// Calculates the requested number of replicas and partitions.
// The explicit num_replicas and num_partitions options override
// execution_options.
// Regarding the num_slices parameter, see the comment on
// xla::MultiSliceConfig.
ReplicasAndPartitions GetReplicasAndPartitions(
    const std::optional<ExecutionOptions>& execution_options, int device_count,
    const std::optional<int>& num_replicas,
    const std::optional<int>& num_partitions, int num_slices = 1) {
  CHECK_GE(num_slices, 1);
  ReplicasAndPartitions result = GetReplicasAndPartitionsInternal(
      execution_options, device_count, num_replicas, num_partitions,
      num_slices);
  VLOG(1) << "Calculated replicas: " << result.replicas
          << ", partitions: " << result.partitions;
  CHECK_GE(result.replicas, 1);
  CHECK_GE(result.partitions, 1);
  return result;
}

absl::StatusOr<PerDeviceLiteralVecType> FetchAndLogOutput(
    PjRtClient& client,
    const std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>& output_buffers,
    ModuleOutputMode module_output_mode, bool log_output) {
  CHECK(!output_buffers.empty());
  absl::Mutex mu;
  absl::Status status;
  size_t num_pending_transfers = 0;
  bool device_0_is_local = false;
  for (PjRtDevice* device : GetLocalDevices(client)) {
    if (device->id() == 0) {
      device_0_is_local = true;
    }
  }

  if (module_output_mode == ModuleOutputMode::kReturnDevice0Outputs &&
      device_0_is_local) {
    num_pending_transfers = output_buffers[0].size();
  } else if (module_output_mode == ModuleOutputMode::kReturnOutputs) {
    for (const auto& bs : output_buffers) {
      num_pending_transfers += bs.size();
    }
  }

  PerDeviceLiteralVecType outputs;
  for (int i = 0; i < output_buffers.size(); ++i) {
    if (output_buffers[i].empty()) {
      continue;
    }
    const int device_id = output_buffers[i][0]->device()->id();
    std::vector<Literal>& output_slice = outputs[device_id];
    if (module_output_mode == ModuleOutputMode::kReturnOutputs ||
        (module_output_mode == ModuleOutputMode::kReturnDevice0Outputs &&
         device_id == 0)) {
      output_slice.reserve(output_buffers[i].size());
      for (const auto& buffer : output_buffers[i]) {
        TF_RET_CHECK(buffer->device() == output_buffers[i][0]->device())
            << "All outputs from a given vector of outputs should be for the "
               "same device";
        output_slice.emplace_back(
            ShapeUtil::DeviceShapeToHostShape(buffer->on_device_shape()));
        buffer->ToLiteral(&output_slice.back()).OnReady([&](absl::Status s) {
          absl::MutexLock lock(&mu);
          --num_pending_transfers;
          status.Update(s);
        });
      }
    } else {
      for (const auto& buffer : output_buffers[i]) {
        TF_RET_CHECK(buffer->device() == output_buffers[i][0]->device())
            << "All outputs from a given vector of outputs should be for the "
               "same device";
        TF_RETURN_IF_ERROR(buffer->GetReadyFuture().Await());
      }
    }
  }
  if (module_output_mode == ModuleOutputMode::kReturnOutputs ||
      (module_output_mode == ModuleOutputMode::kReturnDevice0Outputs &&
       device_0_is_local)) {
    auto cond = [&]() { return !status.ok() || num_pending_transfers == 0; };
    absl::MutexLock lock(&mu);
    mu.Await(absl::Condition(&cond));
    TF_RETURN_IF_ERROR(status);
    if (log_output) {
      for (const PjRtDevice* device : GetLocalDevices(client)) {
        int device_id = device->id();
        if (module_output_mode == ModuleOutputMode::kReturnDevice0Outputs &&
            device_id != 0) {
          continue;
        }
        LOG(INFO) << "Outputs for device_id: " << device_id;
        const std::vector<Literal>& output_slice = outputs[device_id];
        for (int i = 0; i < output_slice.size(); ++i) {
          LOG(INFO) << "output[" << i << "]: " << output_slice[i].ToString();
        }
      }
    }
  }
  return outputs;
}

std::vector<std::vector<PjRtBuffer*>> CreateArgumentPointersFromDeviceBuffers(
    absl::Span<const std::vector<std::unique_ptr<PjRtBuffer>>> device_buffers) {
  std::vector<std::vector<PjRtBuffer*>> argument_ptrs(device_buffers.size());
  for (int i = 0; i < device_buffers.size(); i++) {
    argument_ptrs[i].resize(device_buffers[i].size());
    for (int j = 0; j < device_buffers[i].size(); j++) {
      argument_ptrs[i][j] = device_buffers[i][j].get();
    }
  }
  return argument_ptrs;
}

std::vector<std::vector<PjRtBuffer*>> CreateArgumentPointersBasedOnAliasing(
    absl::Span<const std::vector<std::unique_ptr<PjRtBuffer>>> output_buffers,
    absl::Span<const std::vector<std::unique_ptr<PjRtBuffer>>> input_buffers,
    std::function<std::optional<int64_t>(int64_t)> get_output_buffer_index) {
  int num_arguments = input_buffers.front().size();
  std::vector<std::vector<PjRtBuffer*>> argument_ptrs(output_buffers.size());
  for (int i = 0; i < input_buffers.size(); i++) {
    argument_ptrs[i].resize(num_arguments);
    for (int argument_index = 0; argument_index < num_arguments;
         argument_index++) {
      std::optional<int> output_buffer_index =
          get_output_buffer_index(argument_index);
      if (!output_buffer_index.has_value()) {
        argument_ptrs[i][argument_index] =
            input_buffers[i][argument_index].get();
      } else {
        argument_ptrs[i][argument_index] =
            output_buffers[i][*output_buffer_index].get();
      }
    }
  }
  return argument_ptrs;
}

std::vector<Shape> GetArgumentShapes(const HloModule& module) {
  const auto& params = module.entry_computation()->parameter_instructions();
  std::vector<Shape> argument_shapes;
  argument_shapes.reserve(params.size());
  for (int i = 0; i < static_cast<int>(params.size()); ++i) {
    const HloModuleConfig& module_config = module.config();
    argument_shapes.push_back((module_config.has_entry_computation_layout() &&
                               module_config.entry_computation_layout()
                                   .parameter_layout(i)
                                   .shape()
                                   .is_static())
                                  ? module_config.entry_computation_layout()
                                        .parameter_layout(i)
                                        .shape()
                                  : params[i]->shape());
  }
  return argument_shapes;
}

absl::Status EnsureSingleTupleForFlattening(const HloModule& module) {
  if (module.entry_computation()->num_parameters() != 1) {
    return InvalidArgument(
        "Flattening arguments requires the number of parameters to be 1. "
        "The actual number of parameters is %d",
        module.entry_computation()->num_parameters());
  }
  if (!module.entry_computation()
           ->parameter_instructions()
           .front()
           ->shape()
           .IsTuple()) {
    return InvalidArgument(
        "Flattening arguments requires the module parameter to be a single "
        "tuple. But the actual parameter shape is %s",
        module.entry_computation()
            ->parameter_instructions()
            .front()
            ->shape()
            .ToString());
  }
  return absl::OkStatus();
}

absl::StatusOr<PerDeviceLiteralVecType> RunInternal(
    PjRtClient& client, PjRtLoadedExecutable* executable,
    std::function<absl::StatusOr<
        std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>(bool)>
        create_argument_buffers_on_device,
    const RunningOptions& running_options) {
  ExecuteOptions execute_options;
  if (running_options.multi_slice_config != nullptr) {
    execute_options.multi_slice_config = running_options.multi_slice_config;
  }
  if (running_options.untuple_result.has_value()) {
    execute_options.untuple_result = *running_options.untuple_result;
  }
  TF_ASSIGN_OR_RETURN(std::vector<std::shared_ptr<HloModule>> hlo_modules,
                      executable->GetHloModules());
  CHECK_EQ(hlo_modules.size(), 1);
  const HloModule& module = *(hlo_modules.front());
  ParameterType parameter_type = GetParameterType(module);
  bool flatten_arguments = parameter_type == ParameterType::kOneTupleOfArrays;
  auto get_output_index_for_one_tuple_of_arrays =
      [&module](int64_t parameter_index) -> std::optional<int64_t> {
    const HloInputOutputAliasConfig& alias_config =
        module.input_output_alias_config();
    std::optional<ShapeIndex> output_index =
        alias_config.GetAliasedOutput(0, {parameter_index});
    if (!output_index.has_value()) {
      return std::nullopt;
    }
    // If the HLO module output is a tuple, it should have been untupled by
    // PjRt. Therefore, we return the tuple index of the buffer.
    if (module.entry_computation()->root_instruction()->shape().IsTuple()) {
      return std::optional<int64_t>(output_index->front());
    }
    CHECK(output_index->empty());
    return 0;
  };
  auto get_output_index_for_one_list_of_arrays =
      [&module](int64_t parameter_index) -> std::optional<int64_t> {
    const HloInputOutputAliasConfig& alias_config =
        module.input_output_alias_config();
    std::optional<ShapeIndex> output_index =
        alias_config.GetAliasedOutput(parameter_index, {});
    if (!output_index.has_value()) {
      return std::nullopt;
    }
    if (module.entry_computation()->root_instruction()->shape().IsTuple()) {
      return std::optional<int64_t>(output_index->front());
    }
    CHECK(output_index->empty());
    return 0;
  };

  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> output_buffers;
  auto output_has_tuple_leaf_on_host_memory_space = [&module]() {
    if (!module.result_shape().IsTuple()) {
      return false;
    }
    return true;
  };
  // If any output leaf buffer is a tuple, PJRT requires untuple_result.
  bool must_untuple_result = output_has_tuple_leaf_on_host_memory_space();
  bool default_untuple_result =
      must_untuple_result || execute_options.untuple_result;
  switch (parameter_type) {
    case ParameterType::kOneTupleOfArrays:
      execute_options.arguments_are_tupled = false;
      execute_options.untuple_result =
          module.entry_computation()->root_instruction()->shape().IsTuple();
      break;
    case ParameterType::kOneListOfArrays:
      execute_options.arguments_are_tupled = false;
      execute_options.untuple_result =
          module.entry_computation()->root_instruction()->shape().IsTuple();
      break;
    case ParameterType::kOther:
      execute_options.arguments_are_tupled = false;
      execute_options.untuple_result = false;
      break;
  }
  if (must_untuple_result) {
    execute_options.untuple_result = true;
  }
  std::optional<std::vector<PjRtFuture<>>> futures;
  futures.emplace();
  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> device_buffers;
  std::vector<std::vector<PjRtBuffer*>> argument_ptrs;
  for (int repeat = 0; repeat < running_options.num_repeats; ++repeat) {
    VLOG(1) << "FunctionalHloRunner: ExecuteOnDevices started (repeat = "
            << repeat << ").";
    {
      XLA_SCOPED_LOGGING_TIMER("FunctionalHloRunner::ExecuteOnDevices");

      if (repeat == 0 || running_options.recreate_buffers_between_repeats) {
        VLOG(1) << "Creating argument buffers. repeat = " << repeat;
        device_buffers.clear();
        argument_ptrs.clear();
        TF_ASSIGN_OR_RETURN(device_buffers, create_argument_buffers_on_device(
                                                flatten_arguments));
        argument_ptrs = CreateArgumentPointersFromDeviceBuffers(device_buffers);
      }
      if (repeat == running_options.num_repeats - 1) {
        execute_options.untuple_result = default_untuple_result;
        if (running_options.profiler != nullptr) {
          running_options.profiler->CreateSession();
        }
      }
      execute_options.launch_id = repeat + 1;
      if (running_options.execution_profiles != nullptr) {
        execute_options.execution_profile =
            &running_options.execution_profiles->emplace_back();
        execute_options.execution_profile->set_warmup_run_executed(repeat > 0);
      }
      futures->clear();
      TF_ASSIGN_OR_RETURN(
          output_buffers,
          executable->Execute(argument_ptrs, execute_options, futures));
      for (auto& future : *futures) {
        TF_RETURN_IF_ERROR(future.Await());
      }
    }
    VLOG(1) << "FunctionalHloRunner: ExecuteOnDevices succeeded (repeat = "
            << repeat << ")";
    if (repeat < running_options.num_repeats - 1) {
      switch (parameter_type) {
        case ParameterType::kOneTupleOfArrays:
          argument_ptrs = CreateArgumentPointersBasedOnAliasing(
              output_buffers, device_buffers,
              get_output_index_for_one_tuple_of_arrays);
          break;
        case ParameterType::kOneListOfArrays:
          argument_ptrs = CreateArgumentPointersBasedOnAliasing(
              output_buffers, device_buffers,
              get_output_index_for_one_list_of_arrays);
          break;
        case ParameterType::kOther:
          argument_ptrs =
              CreateArgumentPointersFromDeviceBuffers(device_buffers);
          break;
      }
    }
  }

  TF_ASSIGN_OR_RETURN(PerDeviceLiteralVecType results,
                      FetchAndLogOutput(client, output_buffers,
                                        running_options.module_output_mode,
                                        running_options.log_input_output()));
  if (running_options.profiler != nullptr) {
    running_options.profiler->UploadSession();
  }
  return results;
}

// Creates argument buffers based on the given arguments map. Note that the
// arguments might be invalid when arguments are destructed.
absl::StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
CopyArgumentsToDevice(PjRtClient& client,
                      const PjRtLoadedExecutable* executable,
                      const PerDeviceLiteralVecType& arguments,
                      const RunningOptions& running_options,
                      bool flattened_arguments,
                      bool clone_device0_arguments = false) {
  const bool log_input = running_options.log_input_output();
  absl::Span<PjRtDevice* const> addressable_devices =
      executable->addressable_devices();
  size_t num_addressable_devices = addressable_devices.size();
  if (!clone_device0_arguments && num_addressable_devices != arguments.size()) {
    return InvalidArgument(
        "The number of provided arguments (%v) does not match the number of "
        "logical devices (%v).",
        arguments.size(), num_addressable_devices);
  }
  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> argument_buffers;
  argument_buffers.resize(num_addressable_devices);

  auto argument_memory_space =
      [&flattened_arguments](const HloModule* module, PjRtDevice* device,
                             int arg_i) -> absl::StatusOr<PjRtMemorySpace*> {
    auto non_tuple_memory_space = [&device](const Shape& shape) {
      if (shape.has_layout() &&
          shape.layout().memory_space() == Layout::kHostMemorySpace) {
        return device->memory_space_by_kind(PinnedHostMemorySpace::kKind);
      }
      return device->default_memory_space();
    };

    const ComputationLayout& entry_layout = module->entry_computation_layout();
    TF_RET_CHECK(entry_layout.parameter_count() > 0);
    if (entry_layout.parameter_shape(0).IsTuple() && flattened_arguments) {
      TF_RET_CHECK(entry_layout.parameter_count() == 1)
          << "entry_layout.parameter_count(): "
          << entry_layout.parameter_count();
      TF_RET_CHECK(arg_i <
                   entry_layout.parameter_shape(0).tuple_shapes().size());
      const Shape& shape = entry_layout.parameter_shape(0).tuple_shapes(arg_i);
      TF_RET_CHECK(!shape.IsTuple()) << "Nested tuples are not supported";
      return non_tuple_memory_space(shape);
    }
    TF_RET_CHECK(arg_i < entry_layout.parameter_count());
    const Shape& shape = entry_layout.parameter_shape(arg_i);
    TF_RET_CHECK(!shape.IsTuple()) << "Param tuple without flattened_arguments";
    return non_tuple_memory_space(shape);
  };
  TF_ASSIGN_OR_RETURN(const std::vector<std::shared_ptr<const PjRtLayout>>&
                          executable_parameter_pjrt_layouts,
                      executable->GetParameterLayouts());
  std::vector<Layout> executable_parameter_layouts;
  executable_parameter_layouts.reserve(
      executable_parameter_pjrt_layouts.size());
  for (const std::shared_ptr<const PjRtLayout>& pjrt_layout :
       executable_parameter_pjrt_layouts) {
    executable_parameter_layouts.push_back(pjrt_layout->xla_layout());
  }
  auto buffer_from_host_literal =
      [&client, &argument_memory_space, &executable_parameter_layouts](
          const HloModule* module, PjRtDevice* device, int arg_i,
          const Literal& literal)
      -> absl::StatusOr<std::unique_ptr<PjRtBuffer>> {
    // Use the layout as specified in the executable rather than the layout of
    // the host-side literal, as the former is the authoritative layout the
    // executable expects.
    const Layout* layout = &executable_parameter_layouts[arg_i];
    TF_ASSIGN_OR_RETURN(PjRtMemorySpace * memory_space,
                        argument_memory_space(module, device, arg_i));
    auto device_buffers =
        client.BufferFromHostLiteral(literal, memory_space, layout);
    // Not all platforms support custom input device layouts. In such cases,
    // we use the only choice i.e. the default layout.
    if (absl::IsUnimplemented(device_buffers.status())) {
      return client.BufferFromHostLiteral(literal, memory_space,
                                          /*device_layout=*/nullptr);
    }
    return device_buffers;
  };

  absl::Span<const PjRtLoadedExecutable::LogicalDeviceIds>
      addressable_device_logical_ids =
          executable->addressable_device_logical_ids();
  TF_ASSIGN_OR_RETURN(std::vector<std::shared_ptr<HloModule>> hlo_modules,
                      executable->GetHloModules());

  for (int i = 0; i < num_addressable_devices; ++i) {
    PjRtDevice* curr_device = addressable_devices[i];
    int curr_device_id = curr_device->id();
    // 'source_device' determines where we get the input literal from.
    PjRtDevice* source_device =
        addressable_devices[clone_device0_arguments ? 0 : i];
    int source_device_id = source_device->id();
    if (!arguments.contains(source_device_id)) {
      return InvalidArgument(
          "The provided argument map does not contain arguments "
          "for device: %d",
          curr_device_id);
    }

    const std::vector<Literal>& curr_device_arguments =
        arguments.at(source_device_id);

    int executable_idx = hlo_modules.size() == 1
                             ? 0
                             : addressable_device_logical_ids[i].partition;
    HloModule* module = hlo_modules[executable_idx].get();

    argument_buffers[i].reserve(curr_device_arguments.size());
    for (int arg_i = 0; arg_i < curr_device_arguments.size(); ++arg_i) {
      const Literal& literal = curr_device_arguments[arg_i];
      if (log_input) {
        LOG(INFO) << "device_id=" << curr_device_id
                  << ", input = " << literal.ToString();
      }
      TF_ASSIGN_OR_RETURN(
          std::unique_ptr<PjRtBuffer> argument_buffer,
          buffer_from_host_literal(module, curr_device, arg_i, literal));
      argument_buffers[i].push_back(std::move(argument_buffer));
    }
  }
  for (const auto& device_argument_buffers : argument_buffers) {
    for (const auto& device_buffer : device_argument_buffers) {
      TF_RETURN_IF_ERROR(device_buffer->GetReadyFuture().Await());
    }
  }
  return argument_buffers;
}

// Creates uninitialized arguments to run the given executable.
absl::StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
CreateUninitializedArgumentsOnDevice(PjRtClient& client,
                                     const PjRtLoadedExecutable* executable,
                                     const RunningOptions& running_options,
                                     bool flatten_arguments = false) {
  absl::Span<PjRtDevice* const> addressable_devices =
      executable->addressable_devices();
  absl::Span<const PjRtLoadedExecutable::LogicalDeviceIds>
      addressable_device_logical_ids =
          executable->addressable_device_logical_ids();
  TF_ASSIGN_OR_RETURN(std::vector<std::shared_ptr<HloModule>> hlo_modules,
                      executable->GetHloModules());
  VLOG(1) << "FunctionalHloRunner: local_executable count = "
          << hlo_modules.size();

  LOG(INFO) << "Starting argument buffer shape calculation.";
  PerDeviceShapeVecType argument_shapes_per_device;
  // This must be true, based on the comment on
  // PjRtLoadedExecutable::addressable_devices().
  CHECK_EQ(addressable_devices.size(), addressable_device_logical_ids.size());
  for (int i = 0; i < static_cast<int>(addressable_devices.size()); ++i) {
    VLOG(3) << "Calculating fake argument shapes for device " << i;
    PjRtDevice* device = addressable_devices[i];
    int executable_idx = hlo_modules.size() == 1
                             ? 0
                             : addressable_device_logical_ids[i].partition;
    const HloModule& hlo_module = *hlo_modules[executable_idx];

    std::vector<Shape> argument_shapes;
    if (flatten_arguments) {
      TF_RETURN_IF_ERROR(EnsureSingleTupleForFlattening(hlo_module));

      std::vector<Shape> original_argument_shapes =
          GetArgumentShapes(hlo_module);
      CHECK_EQ(original_argument_shapes.size(), 1);
      CHECK(original_argument_shapes.front().IsTuple());
      argument_shapes = original_argument_shapes.front().tuple_shapes();
    } else {
      argument_shapes = GetArgumentShapes(hlo_module);
    }

    argument_shapes_per_device[device->id()] = std::move(argument_shapes);
  }

  LOG(INFO) << "Starting argument buffer allocation.";
  int buffer_count = 0;
  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>
      argument_buffers_per_device;
  argument_buffers_per_device.reserve(addressable_devices.size());
  for (int i = 0; i < static_cast<int>(addressable_devices.size()); ++i) {
    VLOG(3) << "Allocating fake arguments for device " << i;
    PjRtDevice* device = addressable_devices[i];
    TF_ASSIGN_OR_RETURN(PjRtMemorySpace * memory_space,
                        device->default_memory_space());

    CHECK(argument_shapes_per_device.contains(device->id()));
    const std::vector<Shape>& argument_shapes =
        argument_shapes_per_device.at(device->id());
    std::vector<std::unique_ptr<PjRtBuffer>> argument_buffers;
    argument_buffers.reserve(argument_shapes.size());

    for (const Shape& shape : argument_shapes) {
      if (running_options.log_input_output()) {
        LOG(INFO) << "device_id=" << device->id()
                  << ", input = " << shape.ToString();
      }

      TF_ASSIGN_OR_RETURN(
          std::unique_ptr<PjRtBuffer> argument_buffer,
          client.CreateUninitializedBuffer(shape, memory_space));
      argument_buffers.push_back(std::move(argument_buffer));
      buffer_count += 1;
    }

    argument_buffers_per_device.push_back(std::move(argument_buffers));
  }
  LOG(INFO) << "Allocated argument buffers: " << buffer_count;

  for (const auto& argument_buffers : argument_buffers_per_device) {
    for (const auto& buffer : argument_buffers) {
      TF_RETURN_IF_ERROR(buffer->GetReadyFuture().Await());
    }
  }
  LOG(INFO) << "Argument buffers are ready.";

  return argument_buffers_per_device;
}

// Creates fake arguments to run the given executable.
absl::StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
CreateArgumentsOnDevice(PjRtClient& client,
                        const PjRtLoadedExecutable* executable,
                        const RunningOptions& running_options,
                        bool flatten_arguments = false,
                        std::minstd_rand0* engine = nullptr) {
  if (running_options.module_argument_mode ==
      ModuleArgumentMode::kUninitialized) {
    return CreateUninitializedArgumentsOnDevice(
        client, executable, running_options, flatten_arguments);
  }

  SlowOperationAlarm alarm(
      absl::Seconds(5),
      absl::StrFormat("Argument initialization is slow. Consider changing "
                      "--hlo_argument_mode."));

  absl::Span<PjRtDevice* const> addressable_devices =
      executable->addressable_devices();
  size_t num_addressable_devices = addressable_devices.size();

  PerDeviceLiteralVecType per_device_argument_literals;
  absl::Span<const PjRtLoadedExecutable::LogicalDeviceIds>
      addressable_device_logical_ids =
          executable->addressable_device_logical_ids();
  TF_ASSIGN_OR_RETURN(std::vector<std::shared_ptr<HloModule>> hlo_modules,
                      executable->GetHloModules());
  VLOG(1) << "FunctionalHloRunner: local_executable count = "
          << hlo_modules.size();

  const bool kUseRandomInputs = running_options.module_argument_mode ==
                                    ModuleArgumentMode::kUseRandomInputs ||
                                running_options.module_argument_mode ==
                                    ModuleArgumentMode::kUseSharedRandomInputs;
  const bool kUseSharedInputs =
      running_options.module_argument_mode ==
          ModuleArgumentMode::kUseSharedRandomInputs ||
      running_options.module_argument_mode ==
          ModuleArgumentMode::kUseZerosAsInput;

  for (int i = 0; i < num_addressable_devices; ++i) {
    VLOG(3) << "Creating fake arguments for device " << i;
    LiteralVec& argument_literals =
        per_device_argument_literals[addressable_devices[i]->id()];
    int executable_idx = hlo_modules.size() == 1
                             ? 0
                             : addressable_device_logical_ids[i].partition;
    HloModule* my_hlo_module = hlo_modules[executable_idx].get();
    if (flatten_arguments) {
      TF_RETURN_IF_ERROR(EnsureSingleTupleForFlattening(*my_hlo_module));
    }
    if (running_options.module_argument_mode ==
        ModuleArgumentMode::kUseDeviceIdAsInput) {
      const auto params =
          my_hlo_module->entry_computation()->parameter_instructions();
      if (flatten_arguments) {
        CHECK_EQ(params.size(), 1);
        CHECK(params.front()->shape().IsTuple());
        argument_literals.reserve(
            params.front()->shape().tuple_shapes().size());
      } else {
        argument_literals.reserve(params.size());
      }
      for (int j = 0; j < params.size(); ++j) {
        TF_ASSIGN_OR_RETURN(
            Literal argument_literal_j,
            MakeFakeLiteralWithSameValue(params[j]->shape(),
                                         addressable_devices[i]->id()));
        if (flatten_arguments) {
          std::vector<Literal> decomposed_argument_literals =
              argument_literal_j.DecomposeTuple();
          for (auto& literal : decomposed_argument_literals) {
            argument_literals.push_back(std::move(literal));
          }
        } else {
          argument_literals.push_back(std::move(argument_literal_j));
        }
      }
    } else {
      if (flatten_arguments) {
        TF_ASSIGN_OR_RETURN(
            LiteralVec tupled_argument_literals,
            MakeFakeArguments(my_hlo_module, kUseRandomInputs,
                              /*use_large_range=*/false,
                              /*treat_gte_as_data_formatting=*/false,
                              /*max_bits_of_precision=*/std::nullopt, engine));
        CHECK_EQ(tupled_argument_literals.size(), 1);
        CHECK(tupled_argument_literals.front().shape().IsTuple());
        argument_literals = tupled_argument_literals.front().DecomposeTuple();
      } else {
        TF_ASSIGN_OR_RETURN(
            argument_literals,
            MakeFakeArguments(my_hlo_module, kUseRandomInputs,
                              /*use_large_range=*/false,
                              /*treat_gte_as_data_formatting=*/false,
                              /*max_bits_of_precision=*/std::nullopt, engine));
      }
      if (kUseSharedInputs) {
        break;
      }
    }
  }

  if (kUseSharedInputs) {
    return CopyArgumentsToDevice(client, executable,
                                 per_device_argument_literals, running_options,
                                 flatten_arguments,
                                 /*clone_device0_arguments=*/true);
  }
  return CopyArgumentsToDevice(client, executable, per_device_argument_literals,
                               running_options, flatten_arguments);
}

// Creates an ExecutableBuildOptions using the specified ExecutionOptions.
ExecutableBuildOptions CreateExecutableBuildOptionsFromExecutionOptions(
    const ExecutionOptions& execution_options) {
  ExecutableBuildOptions build_options;
  if (execution_options.has_debug_options()) {
    *build_options.mutable_debug_options() = execution_options.debug_options();
    build_options.mutable_debug_options()->set_xla_dump_to("");
  }
  if (execution_options.has_shape_with_output_layout()) {
    absl::StatusOr<Shape> shape =
        Shape::FromProto(execution_options.shape_with_output_layout());
    TF_CHECK_OK(shape.status());
    build_options.set_result_layout(*shape);
  }
  build_options.set_num_replicas(execution_options.num_replicas());
  build_options.set_num_partitions(execution_options.num_partitions());
  build_options.set_use_spmd_partitioning(
      execution_options.use_spmd_partitioning());
  build_options.set_use_shardy_partitioner(
      execution_options.use_shardy_partitioner());
  build_options.set_use_auto_spmd_partitioning(
      execution_options.use_auto_spmd_partitioning());
  build_options.set_deduplicate_hlo(execution_options.deduplicate_hlo());
  build_options.set_allow_spmd_sharding_propagation_to_parameters(
      execution_options.allow_spmd_sharding_propagation_to_parameters());
  build_options.set_allow_spmd_sharding_propagation_to_output(
      execution_options.allow_spmd_sharding_propagation_to_output());
  if (execution_options.has_device_assignment()) {
    absl::StatusOr<std::unique_ptr<DeviceAssignment>> device_assignment =
        DeviceAssignment::Deserialize(execution_options.device_assignment());
    TF_CHECK_OK(device_assignment.status());
    build_options.set_device_assignment(**device_assignment);
  }
  build_options.set_alias_passthrough_params(
      execution_options.alias_passthrough_params());
  return build_options;
}

}  // namespace

absl::StatusOr<ExecutionOptions> LoadExecutionOptions(absl::string_view path) {
  ExecutionOptions execution_options;
  TF_RETURN_IF_ERROR(tsl::ReadTextOrBinaryProto(
      tsl::Env::Default(), std::string(path), &execution_options));
  return execution_options;
}

absl::StatusOr<CompileOptions> CreateCompileOptions(
    const PjRtClient& client,
    const FunctionalHloRunner::RawCompileOptions& raw_options, int task_id,
    int num_nodes, std::shared_ptr<xla::KeyValueStoreInterface> kv_store) {
  CompileOptions compile_options;
  if (raw_options.execution_options.has_value()) {
    compile_options.executable_build_options =
        CreateExecutableBuildOptionsFromExecutionOptions(
            raw_options.execution_options.value());
  }

  ExecutableBuildOptions& build_options =
      compile_options.executable_build_options;
  ReplicasAndPartitions replicas_and_partitions =
      FunctionalHloRunner::GetReplicasAndPartitions(
          raw_options.execution_options, client.device_count(),
          raw_options.num_replicas, raw_options.num_partitions,
          raw_options.num_slices.value_or(1));
  build_options.set_num_replicas(replicas_and_partitions.replicas);
  build_options.set_num_partitions(replicas_and_partitions.partitions);
  build_options.set_process_index(task_id);
  build_options.set_process_count(num_nodes);
  build_options.set_key_value_store(kv_store);
  if (raw_options.spmd_mode == SpmdMode::kUseSpmdPartitioning ||
      raw_options.spmd_mode == SpmdMode::kUseShardyPartitioning) {
    build_options.set_use_spmd_partitioning(true);
    if (raw_options.spmd_mode == SpmdMode::kUseShardyPartitioning) {
      build_options.set_use_shardy_partitioner(true);
    }
  }
  if (!build_options.has_device_assignment() &&
      !raw_options.num_slices.has_value()) {
    TF_ASSIGN_OR_RETURN(
        DeviceAssignment device_assignment,
        client.GetDefaultDeviceAssignment(replicas_and_partitions.replicas,
                                          replicas_and_partitions.partitions));
    build_options.set_device_assignment(device_assignment);
  }
  DebugOptions& debug_options = *build_options.mutable_debug_options();
  if (task_id == 0) {
    // Overwrite xla_dump_to only if it's not empty, to preserve `xla_dump_to`
    // from parsed XLA_FLAGS env (already populated in debug_options).
    if (!raw_options.xla_dump_to.empty()) {
      debug_options.set_xla_dump_to(raw_options.xla_dump_to);
      debug_options.set_xla_dump_hlo_as_text(raw_options.xla_text_dump_mode ==
                                             XlaTextDumpMode::kDumpAsText);
      debug_options.set_xla_dump_hlo_as_proto(raw_options.xla_proto_dump_mode ==
                                              XlaProtoDumpMode::kDumpAsProto);
    }
  }
  switch (raw_options.hlo_passes_mode) {
    case HloPassesMode::kRunXLABackendOnly:
      build_options.set_run_backend_only(true);
      break;
    case HloPassesMode::kDisableAllHloPasses:
      debug_options.set_xla_disable_all_hlo_passes(true);
      break;
    case HloPassesMode::kStandardCompile:
      // Just use the default.
      break;
  }
  return compile_options;
}

absl::Status DumpOutput(
    const FunctionalHloRunner::PerDeviceLiteralVecType& output,
    absl::string_view dump_output_to, int task_id, OutputFormat output_format) {
  std::vector<std::string> output_path_vec =
      absl::StrSplit(dump_output_to, '.');
  std::string suffix = output_path_vec.back();
  output_path_vec.pop_back();
  output_path_vec.push_back(absl::StrCat("task_", task_id));
  output_path_vec.push_back("");
  int device_id_index = output_path_vec.size() - 1;
  output_path_vec.push_back("");
  int literal_id_index = output_path_vec.size() - 1;
  output_path_vec.push_back(suffix);
  for (const auto& [device_id, literal_vec] : output) {
    output_path_vec[device_id_index] = absl::StrCat("device_", device_id);
    for (int literal_id = 0; literal_id < literal_vec.size(); ++literal_id) {
      output_path_vec[literal_id_index] = absl::StrCat("literal_", literal_id);
      std::string literal_path = absl::StrJoin(output_path_vec, ".");
      switch (output_format) {
        case OutputFormat::kText: {
          CHECK_EQ(suffix, std::string("txt"));
          absl::Status write_status =
              tsl::WriteStringToFile(tsl::Env::Default(), literal_path,
                                     literal_vec[literal_id].ToString());
          if (!write_status.ok()) {
            return write_status;
          }
          break;
        }
        case OutputFormat::kProtoBinary: {
          CHECK_EQ(suffix, std::string("pb"));
          TF_RETURN_IF_ERROR(
              tsl::WriteBinaryProto(tsl::Env::Default(), literal_path,
                                    literal_vec[literal_id].ToProto()));
          break;
        }
        case OutputFormat::kProtoText: {
          CHECK_EQ(suffix, std::string("pbtxt"));
          TF_RETURN_IF_ERROR(
              tsl::WriteTextProto(tsl::Env::Default(), literal_path,
                                  literal_vec[literal_id].ToProto()));
          break;
        }
      }
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<HloModuleAndArguments> LoadHloModuleAndArguments(
    absl::string_view hlo_file, InputFormat input_format) {
  HloModuleAndArguments hlo_module_and_arguments;
  switch (input_format) {
    case InputFormat::kText: {
      TF_ASSIGN_OR_RETURN(
          hlo_module_and_arguments.hlo_module,
          ReadModuleFromHloTextFile(
              hlo_file, DebugOptions::default_instance(),
              HloParserOptions().set_keep_module_auto_layouts(true)));
      break;
    }
    case InputFormat::kProtoText: {
      TF_ASSIGN_OR_RETURN(hlo_module_and_arguments.hlo_module,
                          ReadModuleFromTextProtoFile(hlo_file));
      break;
    }
    case InputFormat::kProtoBinary: {
      TF_ASSIGN_OR_RETURN(hlo_module_and_arguments.hlo_module,
                          ReadModuleFromBinaryProtoFile(hlo_file));
      break;
    }
    case InputFormat::kSnapshotProtoBinary: {
      TF_ASSIGN_OR_RETURN(hlo_module_and_arguments,
                          ReadModuleFromSnapshotBinaryProtoFile(hlo_file));
      break;
    }
    case InputFormat::kUnoptimizedSnapshotProtoBinary: {
      TF_ASSIGN_OR_RETURN(
          hlo_module_and_arguments,
          ReadModuleFromUnoptimizedSnapshotBinaryProtoFile(hlo_file));
      break;
    }
    case InputFormat::kUnoptimizedSnapshotProtoText: {
      TF_ASSIGN_OR_RETURN(
          hlo_module_and_arguments,
          ReadModuleFromUnoptimizedSnapshotTextProtoFile(hlo_file));
      break;
    }
    case InputFormat::kSerializedPjRtExecutable: {
      LOG(INFO) << "Skipping loading HLO module and arguments for serialized "
                   "PjRtExecutable.";
      return hlo_module_and_arguments;
      break;
    }
    default:
      LOG(FATAL) << "Cannot process input format: "
                 << AbslUnparseFlag(input_format);
  }
  return hlo_module_and_arguments;
}

absl::Status LoadAndRunAndDump(
    PjRtClient& client, const DebugOptions& debug_options,
    const xla::FunctionalHloRunner::PreprocessingOptions& preproc_options,
    const xla::FunctionalHloRunner::RawCompileOptions& raw_compile_options,
    const xla::FunctionalHloRunner::RunningOptions& running_options,
    absl::string_view hlo_file, InputFormat input_format,
    std::string dump_output_to, int task_id, int num_nodes,
    std::shared_ptr<xla::KeyValueStoreInterface> kv_store) {
  TF_ASSIGN_OR_RETURN(
      CompileOptions compile_options,
      FunctionalHloRunner::CreateCompileOptions(client, raw_compile_options,
                                                task_id, num_nodes, kv_store));
  TF_ASSIGN_OR_RETURN(
      FunctionalHloRunner::PerDeviceLiteralVecType output,
      FunctionalHloRunner::LoadAndRun(client, debug_options, preproc_options,
                                      compile_options, running_options,
                                      hlo_file, input_format));
  return dump_output_to.empty()
             ? absl::OkStatus()
             : FunctionalHloRunner::DumpOutput(output, dump_output_to, task_id);
}

absl::StatusOr<FunctionalHloRunner::PerDeviceLiteralVecType> LoadAndRun(
    PjRtClient& client, const DebugOptions& debug_options,
    const PreprocessingOptions& preproc_options,
    const CompileOptions& compile_options,
    const RunningOptions& running_options, absl::string_view hlo_file,
    InputFormat input_format, const PerDeviceLiteralVecType& arguments,
    std::minstd_rand0* engine) {
  // We only support SPMD as of now, i.e., all devices are supposed
  // to execute the same HLO module.
  // Currently there is no mechanism to map the loaded arguments to
  // proper device ID, so loading and executing from HLO snapshot might not
  // replay the original execution.

  const PerDeviceLiteralVecType* final_arguments = nullptr;
  std::unique_ptr<HloModule> hlo_module;
  PerDeviceLiteralVecType loaded_arguments;
  if (!arguments.empty()) {
    final_arguments = &arguments;
  } else {
    TF_ASSIGN_OR_RETURN(HloModuleAndArguments hlo_module_and_arguments,
                        LoadHloModuleAndArguments(hlo_file, input_format));

    // Check that the number of shards is not greater than the number of
    // devices.
    if (hlo_module_and_arguments.arguments.size() > client.devices().size()) {
      return absl::InvalidArgumentError(
          "The number of shards in the given input file is greater than the "
          "number of devices available on the host.");
    }

    for (int i = 0; i < hlo_module_and_arguments.arguments.size(); ++i) {
      loaded_arguments[client.devices()[i]->id()] =
          std::move(hlo_module_and_arguments.arguments[i]);
    }
    hlo_module = std::move(hlo_module_and_arguments.hlo_module);
    final_arguments = &loaded_arguments;
  }

  if (input_format == InputFormat::kSerializedPjRtExecutable) {
    std::string serialized_executable;
    TF_RETURN_IF_ERROR(tsl::ReadFileToString(
        tsl::Env::Default(), std::string(hlo_file), &serialized_executable));
    TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtLoadedExecutable> executable,
                        client.LoadSerializedExecutable(
                            serialized_executable,
                            /*options=*/std::nullopt, LoadOptions()));
    return Run(client, executable.get(), *final_arguments, running_options,
               engine);
  }
  if (!hlo_module) {
    // Load hlo module.
    TF_ASSIGN_OR_RETURN(HloModuleAndArguments hlo_module_and_arguments,
                        LoadHloModuleAndArguments(hlo_file, input_format));
    hlo_module = std::move(hlo_module_and_arguments.hlo_module);
  }

  return CompileAndRun(client, debug_options, preproc_options, compile_options,
                       running_options, hlo_module.get(), *final_arguments,
                       engine);
}

absl::Status LoadAndCompile(
    PjRtClient& client, const DebugOptions& debug_options,
    const PreprocessingOptions& preproc_options,
    const RawCompileOptions& raw_compile_options, absl::string_view hlo_file,
    InputFormat input_format, int task_id, int num_nodes,
    std::shared_ptr<xla::KeyValueStoreInterface> kv_store,
    bool use_gpu_count_workaround) {
  TF_ASSIGN_OR_RETURN(
      CompileOptions compile_options,
      FunctionalHloRunner::CreateCompileOptions(client, raw_compile_options,
                                                task_id, num_nodes, kv_store));

  int num_replicas = compile_options.executable_build_options.num_replicas();
  int num_partitions =
      compile_options.executable_build_options.num_partitions();
  int needed_devices = num_replicas * num_partitions;
  if (client.addressable_device_count() < needed_devices &&
      use_gpu_count_workaround) {
    LOG(INFO) << "Applying a workaround to allow compiling multi-device HLOs "
                 "on machines with fewer devices.";
    DeviceAssignment assignment(num_replicas, num_partitions);
    assignment.Fill(0);
    compile_options.executable_build_options.set_device_assignment(assignment);
  }

  TF_ASSIGN_OR_RETURN(HloModuleAndArguments hlo_module_and_arguments,
                      LoadHloModuleAndArguments(hlo_file, input_format));

  TF_RETURN_IF_ERROR(FunctionalHloRunner::Compile(
                         client, hlo_module_and_arguments.hlo_module.get(),
                         debug_options, preproc_options, compile_options)
                         .status());

  return absl::OkStatus();
}

absl::StatusOr<FunctionalHloRunner::PerDeviceLiteralVecType> CompileAndRun(
    PjRtClient& client, const DebugOptions& debug_options,
    const PreprocessingOptions& preproc_options,
    const CompileOptions& compile_options,
    const RunningOptions& running_options, HloModule* hlo_module,
    const PerDeviceLiteralVecType& arguments, std::minstd_rand0* engine) {
  TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtLoadedExecutable> executable,
                      Compile(client, hlo_module, debug_options,
                              preproc_options, compile_options));

  return Run(client, executable.get(), arguments, running_options, engine);
}

absl::Status PrepareHloModuleForCompilation(
    HloModule* hlo_module, const DebugOptions& debug_options,
    const PreprocessingOptions& preproc_options) {
  hlo_module->mutable_config().set_debug_options(debug_options);

  if (preproc_options.is_spmd_partitioned_module()) {
    // If the module has already been partitioned by SPMD, add sharding
    // annotations (replicated) to module parameters and result.
    AddShardingAnnotationsToSpmdPartitionedModule(hlo_module);
  }

  if (preproc_options.flatten_while_loop() ||
      preproc_options.remove_infeed_outfeed ||
      preproc_options.flatten_conditional) {
    // The pipeline will check for the presence of
    // debug_options().xla_disable_hlo_passes().
    HloPassPipeline pipeline("control-flow-flattening-pipeline");
    int while_execution_count =
        preproc_options.while_execution_count.value_or(0);
    pipeline.AddPass<HloControlFlowFlattening>(
        HloControlFlowFlattening::Options{
            /*while_execution_count=*/while_execution_count,
            /*max_outer_loop_count=*/
            while_execution_count,
            /*max_loop_count=*/
            while_execution_count,
            /*remove_infeed_outfeed=*/preproc_options.remove_infeed_outfeed,
            /*flatten_while_loop=*/preproc_options.flatten_while_loop(),
            /*remove_comm=*/false,
            /*remove_host_transfer=*/true,
            /*remove_id=*/false,
            /*flatten_conditional=*/
            preproc_options.flatten_conditional,
            /*conditional_value=*/
            preproc_options.conditional_value});
    if (preproc_options.annotate_while_loop_trip_count) {
      pipeline.AddPass<WhileLoopTripCountAnnotator>();
    }
    TF_RETURN_IF_ERROR(pipeline.Run(hlo_module).status());
  }
  return absl::OkStatus();
}

absl::StatusOr<CompileOptions> CompleteCompileOptions(
    const HloModule& hlo_module, CompileOptions compile_options,
    const PreprocessingOptions& preproc_options) {
  ParameterType parameter_type = GetParameterType(hlo_module);
  compile_options.parameter_is_tupled_arguments =
      (parameter_type == ParameterType::kOneTupleOfArrays);
  if (preproc_options.force_auto_layout) {
    XlaComputation computation(hlo_module.ToProto());
    TF_ASSIGN_OR_RETURN(ProgramShape program_shape,
                        computation.GetProgramShape());
    LayoutUtil::ClearLayout(&program_shape);
    compile_options.argument_layouts = program_shape.parameters();
    compile_options.executable_build_options.set_result_layout(
        program_shape.result());
    compile_options.executable_build_options.mutable_debug_options()
        ->set_xla_pjrt_allow_auto_layout_in_hlo(true);
  } else if (preproc_options.use_layouts_from_hlo_module) {
    const ComputationLayout& layout = hlo_module.entry_computation_layout();
    std::vector<Shape> parameter_shapes;
    parameter_shapes.reserve(layout.parameter_count());
    for (const ShapeLayout& shape_layout : layout.parameter_layouts()) {
      parameter_shapes.push_back(shape_layout.shape());
    }
    compile_options.argument_layouts = std::move(parameter_shapes);
    compile_options.executable_build_options.set_result_layout(
        layout.result_shape());
    compile_options.executable_build_options.mutable_debug_options()
        ->set_xla_pjrt_allow_auto_layout_in_hlo(true);
  }
  return compile_options;
}

namespace {

// Depending on the compile_as_stablehlo flag, convert the HLO module either to
// StableHLO mlir::Module or to XlaComputation and calls the compile_function
// which should take either of these as input.
template <typename R, typename T>
absl::StatusOr<std::unique_ptr<R>> ConvertAndCallCompiler(
    bool compile_as_stablehlo, HloModule* hlo_module, T&& compile_function) {
  auto compile_and_log =
      [&](const auto& module) -> absl::StatusOr<std::unique_ptr<R>> {
    VLOG(1) << "FunctionalHloRunner: compilation started.";
    TF_ASSIGN_OR_RETURN(auto result, compile_function(module));
    VLOG(1) << "FunctionalHloRunner: compile succeeded.";
    return result;
  };

  if (compile_as_stablehlo) {
    mlir::DialectRegistry registry;
    mlir::func::registerAllExtensions(registry);
    mlir::MLIRContext context(registry);
    TF_ASSIGN_OR_RETURN(mlir::OwningOpRef<mlir::ModuleOp> stablehlo_module,
                        ConvertHloToStablehlo(context, hlo_module));
    return compile_and_log(*stablehlo_module);
  } else {
    XlaComputation computation(hlo_module->ToProto());
    return compile_and_log(computation);
  }
}

}  // namespace

absl::StatusOr<std::unique_ptr<PjRtLoadedExecutable>> Compile(
    PjRtClient& client, HloModule* hlo_module,
    const DebugOptions& debug_options,
    const PreprocessingOptions& preproc_options,
    const CompileOptions& compile_options) {
  TF_RETURN_IF_ERROR(PrepareHloModuleForCompilation(hlo_module, debug_options,
                                                    preproc_options));
  TF_ASSIGN_OR_RETURN(
      CompileOptions modified_compile_options,
      CompleteCompileOptions(*hlo_module, compile_options, preproc_options));

  return ConvertAndCallCompiler<PjRtLoadedExecutable>(
      preproc_options.compile_as_stablehlo, hlo_module,
      [&](const auto& module) {
        return client.CompileAndLoad(module, modified_compile_options);
      });
}

absl::StatusOr<std::unique_ptr<PjRtExecutable>> Compile(
    PjRtClient& client, HloModule* hlo_module,
    const DebugOptions& debug_options,
    const PreprocessingOptions& preproc_options,
    const CompileOptions& compile_options,
    const PjRtTopologyDescription& topology) {
  TF_RETURN_IF_ERROR(PrepareHloModuleForCompilation(hlo_module, debug_options,
                                                    preproc_options));
  TF_ASSIGN_OR_RETURN(
      CompileOptions modified_compile_options,
      CompleteCompileOptions(*hlo_module, compile_options, preproc_options));

  return ConvertAndCallCompiler<PjRtExecutable>(
      preproc_options.compile_as_stablehlo, hlo_module,
      [&](const auto& module) {
        return PjRtCompile(modified_compile_options, module, topology, &client);
      });
}

// Runs the executable and may repeat for multiple times.
absl::StatusOr<FunctionalHloRunner::PerDeviceLiteralVecType> Run(
    PjRtClient& client, PjRtLoadedExecutable* executable,
    const PerDeviceLiteralVecType& arguments,
    const RunningOptions& running_options, std::minstd_rand0* engine) {
  auto create_argument_buffers_on_device = [&client, &executable, &arguments,
                                            &running_options, engine](
                                               bool flatten_tupled_arguments) {
    if (arguments.empty()) {
      return CreateArgumentsOnDevice(client, executable, running_options,
                                     flatten_tupled_arguments, engine);
    }

    if (flatten_tupled_arguments && arguments.begin()->second.size() == 1 &&
        arguments.begin()->second.front().shape().IsTuple()) {
      PerDeviceLiteralVecType flattened_arguments;
      for (const auto& device_id_and_arguments : arguments) {
        Literal tupled_argument =
            device_id_and_arguments.second.front().Clone();
        LiteralVec flattened_argument = tupled_argument.DecomposeTuple();
        int device_id = device_id_and_arguments.first;
        flattened_arguments.insert({device_id, std::move(flattened_argument)});
      }
      return CopyArgumentsToDevice(client, executable, flattened_arguments,
                                   running_options,
                                   /*flattened_arguments=*/true);
    }
    // If the per-device argument is not a single tuple, we ignore the
    // flatten_tupled_arguments parameter and assume the provided arguments have
    // already been flattened.
    return CopyArgumentsToDevice(client, executable, arguments, running_options,
                                 /*flattened_arguments=*/false);
  };
  return RunInternal(client, executable, create_argument_buffers_on_device,
                     running_options);
}

namespace {
const FixedOptionSetFlagParser<FunctionalHloRunner::ModuleOutputMode>&
GetModuleOutputModeParser() {
  using ModuleOutputMode = FunctionalHloRunner::ModuleOutputMode;
  static const auto& parser = GetFixedOptionSetFlagParser<ModuleOutputMode>(
      {{"return_outputs", ModuleOutputMode::kReturnOutputs},
       {"not_return_outputs", ModuleOutputMode::kNotReturnOutputs},
       {"return_device_0_outputs", ModuleOutputMode::kReturnDevice0Outputs}});
  return parser;
}
const FixedOptionSetFlagParser<FunctionalHloRunner::ModuleArgumentMode>&
GetModuleArgumentModeParser() {
  using ModuleArgumentMode = FunctionalHloRunner::ModuleArgumentMode;
  static const auto& parser = GetFixedOptionSetFlagParser<ModuleArgumentMode>(
      {{"use_device_id_as_input", ModuleArgumentMode::kUseDeviceIdAsInput},
       {"use_random_inputs", ModuleArgumentMode::kUseRandomInputs},
       {"use_shared_random_inputs", ModuleArgumentMode::kUseSharedRandomInputs},
       {"use_zeros_as_input", ModuleArgumentMode::kUseZerosAsInput},
       {"uninitialized", ModuleArgumentMode::kUninitialized}});
  return parser;
}
}  // namespace

bool AbslParseFlag(absl::string_view text, ModuleArgumentMode* argument_mode,
                   std::string* error) {
  return GetModuleArgumentModeParser().Parse(text, argument_mode, error);
}

std::string AbslUnparseFlag(ModuleArgumentMode argument_mode) {
  return GetModuleArgumentModeParser().Unparse(argument_mode);
}

bool AbslParseFlag(absl::string_view text, ModuleOutputMode* output_mode,
                   std::string* error) {
  return GetModuleOutputModeParser().Parse(text, output_mode, error);
}

std::string AbslUnparseFlag(ModuleOutputMode output_mode) {
  return GetModuleOutputModeParser().Unparse(output_mode);
}

}  // namespace FunctionalHloRunner

HLORunnerProfiler::HLORunnerProfiler(absl::string_view dump_path,
                                     bool keep_xspace)
    : dump_path_(dump_path), keep_xspace_(keep_xspace) {}

absl::StatusOr<std::unique_ptr<HLORunnerProfiler>> HLORunnerProfiler::Create(
    absl::string_view dump_path, bool keep_xspace) {
  if (dump_path.empty()) {
    return absl::InvalidArgumentError(
        "Please provide a valid dump path to save XSpace results to disk.");
  }
  return std::make_unique<HLORunnerProfiler>(dump_path, keep_xspace);
}

void HLORunnerProfiler::CreateSession() {
  auto options = tsl::ProfilerSession::DefaultOptions();
  session_ = tsl::ProfilerSession::Create(options);
}

void HLORunnerProfiler::UploadSession() {
  xspace_ = std::make_unique<tensorflow::profiler::XSpace>();
  // Stops the ProfilerSession
  TF_CHECK_OK(session_->CollectData(xspace_.get()));

  CHECK(!dump_path_.empty());

  LOG(INFO) << "Saving xspace result to " << dump_path_;
  // Save in binary format to create xprof sessions and extract device stats.
  CHECK_OK(WriteBinaryProto(tsl::Env::Default(), dump_path_, *xspace_.get()));
  if (!keep_xspace_) {
    xspace_ = nullptr;
  }
}

const tensorflow::profiler::XSpace* HLORunnerProfiler::GetXSpace() {
  return xspace_.get();
}

void AddShardingAnnotationsToSpmdPartitionedModule(HloModule* hlo_module) {
  auto set_manual_sharding = [](HloInstruction* hlo) {
    if (!hlo->has_sharding()) {
      hlo->set_sharding(
          HloSharding::Manual().NormalizeTupleSharding(hlo->shape()));
    }
  };
  for (int64_t i = 0; i < hlo_module->entry_computation()->num_parameters();
       ++i) {
    HloInstruction* param =
        hlo_module->entry_computation()->parameter_instruction(i);
    set_manual_sharding(param);
  }

  HloInstruction* entry_root =
      hlo_module->entry_computation()->root_instruction();
  set_manual_sharding(entry_root);
}

}  // namespace xla
