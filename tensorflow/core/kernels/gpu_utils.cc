/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/kernels/gpu_utils.h"

#if GOOGLE_CUDA

#include <iterator>

#include "google/protobuf/any.pb.h"
#include "absl/algorithm/container.h"
#include "tensorflow/core/platform/logger.h"
#include "tensorflow/core/protobuf/autotuning.pb.h"
#include "tensorflow/core/protobuf/conv_autotuning.pb.h"
#include "tensorflow/core/util/proto/proto_utils.h"
#include "tensorflow/stream_executor/cuda/ptxas_utils.h"
#include "tensorflow/stream_executor/cuda/redzone_allocator.h"
#include "tensorflow/stream_executor/cuda/cuda_helpers.h"

namespace tensorflow {

bool RedzoneCheckDisabled() {
  const char* disable_rz_str = std::getenv("TF_DISABLE_RZ_CHECK");
  return disable_rz_str != nullptr && std::strcmp(disable_rz_str, "1") == 0;
}

se::DeviceMemoryBase WrapRedzoneBestEffort(
    se::cuda::RedzoneAllocator* rz_allocator, se::DeviceMemoryBase buffer) {
  if (RedzoneCheckDisabled()) {
    return buffer;
  }
  se::DeviceMemoryBase output_tensor;
  auto output_rz_or = rz_allocator->AllocateBytes(buffer.size());
  if (!output_rz_or.ok()) {
    static std::once_flag rz_allocation_failure_logged;
    std::call_once(rz_allocation_failure_logged, []() {
      LOG(WARNING) << "Failed to allocate memory for convolution redzone "
                   << "checking; skipping this check. This is benign and only "
                   << "means that we won't check cudnn for out-of-bounds reads "
                   << "and writes. This message will only be printed once.";
    });
    return buffer;
  }
  return se::DeviceMemoryBase(output_rz_or.ValueOrDie());
}

template<typename T>
void CheckRedzones(const se::cuda::RedzoneAllocator& rz_allocator,
                   T* autotune_result) {
  if (RedzoneCheckDisabled()) {
    return;
  }
  se::port::StatusOr<se::cuda::RedzoneAllocator::RedzoneCheckStatus> rz_status =
      rz_allocator.CheckRedzones();
  if (!rz_status.ok()) {
    static std::once_flag failure_logged;
    std::call_once(failure_logged, [&]() {
      LOG(WARNING) << "Failed to check cudnn convolutions for out-of-bounds "
                   << "reads and writes with an error message: '"
                   << rz_status.status().error_message()
                   << "'; skipping this check. This only means that we won't "
                   << "check cudnn for out-of-bounds reads and writes. This "
                   << "message will only be printed once.";
    });
    return;
  }
  auto rz_check_status = rz_status.ValueOrDie();
  if (!rz_check_status.ok()) {
    auto* fail = autotune_result->mutable_failure();
    fail->set_msg(rz_check_status.RedzoneFailureMsg());
    fail->set_kind(T::REDZONE_MODIFIED);
    fail->set_buffer_address(
        reinterpret_cast<uint64>(rz_check_status.user_buffer_address));
    LOG(ERROR)
        << "Detected cudnn out-of-bounds write in convolution buffer! This is "
           "likely a cudnn bug. We will skip this algorithm in the future, but "
           "your GPU state may already be corrupted, leading to incorrect "
           "results. Within Google, no action is needed on your part. Outside "
           "of Google, please ensure you're running the latest version of "
           "cudnn. If that doesn't fix the problem, please file a bug with "
           "this full error message and we'll contact nvidia.";
    LOG(ERROR) << rz_check_status.RedzoneFailureMsg();
  }
}

// explicit instantiation
template void CheckRedzones<tensorflow::AutotuneResult>(
    const se::cuda::RedzoneAllocator&, tensorflow::AutotuneResult*);

template void CheckRedzones<tensorflow::AutotuneExecutionPlanResult>(
    const se::cuda::RedzoneAllocator&,
    tensorflow::AutotuneExecutionPlanResult* autotune_result);

namespace {

tensorflow::CudnnVersion GetCudnnVersion(se::StreamExecutor* stream_executor) {
  tensorflow::CudnnVersion cudnn_version;
  if (auto* dnn = stream_executor->AsDnn()) {
    se::port::StatusOr<se::dnn::VersionInfo> version_or = dnn->GetVersion();
    if (version_or.ok()) {
      const auto& version = version_or.ValueOrDie();
      cudnn_version.set_major(version.major_version());
      cudnn_version.set_minor(version.minor_version());
      cudnn_version.set_patch(version.patch());
    }
  }
  return cudnn_version;
}

tensorflow::ComputeCapability GetComputeCapability(
    se::StreamExecutor* stream_executor) {
  tensorflow::ComputeCapability cc;
  int cc_major, cc_minor;
  stream_executor->GetDeviceDescription().cuda_compute_capability(&cc_major,
                                                                  &cc_minor);
  cc.set_major(cc_major);
  cc.set_minor(cc_minor);
  return cc;
}

template<typename TResult, typename TLog>
void LogConvAutotuneResultsImpl(se::dnn::ConvolutionKind kind,
    se::dnn::DataType element_type, se::DeviceMemoryBase input_buffer,
    se::DeviceMemoryBase filter_buffer, se::DeviceMemoryBase output_buffer,
    const se::dnn::BatchDescriptor& input_desc,
    const se::dnn::FilterDescriptor& filter_desc,
    const se::dnn::BatchDescriptor& output_desc,
    const se::dnn::ConvolutionDescriptor& conv_desc,
    se::StreamExecutor* stream_exec,
    absl::Span<const TResult> results) {
  TLog log;
  {
    ConvolutionProto instr;
    instr.set_kind(kind);
    *instr.mutable_input() = input_desc.ToProto(element_type);
    *instr.mutable_filter() = filter_desc.ToProto(element_type);
    *instr.mutable_output() = output_desc.ToProto(element_type);
    *instr.mutable_conv_desc() = conv_desc.ToProto();
    instr.set_conv_scale(1);
    instr.set_side_value_scale(0);
    instr.set_input_address(reinterpret_cast<uint64>(input_buffer.opaque()));
    instr.set_filter_address(reinterpret_cast<uint64>(filter_buffer.opaque()));
    instr.set_output_address(reinterpret_cast<uint64>(output_buffer.opaque()));
    log.mutable_instr()->PackFrom(std::move(instr));
  }
  *log.mutable_cudnn_version() = GetCudnnVersion(stream_exec);
  *log.mutable_compute_capability() = GetComputeCapability(stream_exec);
  log.set_device_pci_bus_id(stream_exec->GetDeviceDescription().pci_bus_id());
  {
    string blas_version;
    if (auto* blas = stream_exec->AsBlas()) {
      if (blas->GetVersion(&blas_version).ok()) {
        log.set_blas_version(blas_version);
      }
    }
  }
  for (const auto& result : results) {
    *log.add_results() = result;
  }
  Logger::GetSingleton()->LogProto(log);
}

template<typename TResult, typename TLog>
void LogFusedConvForwardAutotuneResultsImpl(
    se::dnn::DataType element_type, se::DeviceMemoryBase input_buffer,
    se::DeviceMemoryBase filter_buffer, se::DeviceMemoryBase output_buffer,
    se::DeviceMemoryBase bias_buffer, se::DeviceMemoryBase side_input_buffer,
    const se::dnn::BatchDescriptor& input_desc,
    const se::dnn::FilterDescriptor& filter_desc,
    const se::dnn::BatchDescriptor& output_desc,
    const se::dnn::ConvolutionDescriptor& conv_desc, double conv_scale,
    double side_value_scale, se::dnn::ActivationMode activation_mode,
    se::StreamExecutor* stream_exec, absl::Span<const TResult> results) {
  TLog log;
  {
    ConvolutionProto instr;
    instr.set_kind(se::dnn::ConvolutionKind::FORWARD_BIAS_ACTIVATION);
    *instr.mutable_input() = input_desc.ToProto(element_type);
    *instr.mutable_filter() = filter_desc.ToProto(element_type);
    *instr.mutable_output() = output_desc.ToProto(element_type);
    *instr.mutable_conv_desc() = conv_desc.ToProto();
    instr.set_conv_scale(conv_scale);
    instr.set_side_value_scale(side_value_scale);
    instr.set_activation(activation_mode);
    instr.set_input_address(reinterpret_cast<uint64>(input_buffer.opaque()));
    instr.set_filter_address(reinterpret_cast<uint64>(filter_buffer.opaque()));
    instr.set_output_address(reinterpret_cast<uint64>(output_buffer.opaque()));
    instr.set_bias_address(reinterpret_cast<uint64>(bias_buffer.opaque()));
    instr.set_side_input_address(
        reinterpret_cast<uint64>(side_input_buffer.opaque()));
    log.mutable_instr()->PackFrom(std::move(instr));
  }
  *log.mutable_cudnn_version() = GetCudnnVersion(stream_exec);
  *log.mutable_compute_capability() = GetComputeCapability(stream_exec);
  log.set_device_pci_bus_id(stream_exec->GetDeviceDescription().pci_bus_id());
  {
    string blas_version;
    if (auto* blas = stream_exec->AsBlas()) {
      if (blas->GetVersion(&blas_version).ok()) {
        log.set_blas_version(blas_version);
      }
    }
  }
  for (const auto& result : results) {
    *log.add_results() = result;
  }
  VLOG(2) << log.DebugString();
  Logger::GetSingleton()->LogProto(log);
}

}  // namespace

void LogConvAutotuneResults(se::dnn::ConvolutionKind kind,
                            se::dnn::DataType element_type,
                            se::DeviceMemoryBase input_buffer,
                            se::DeviceMemoryBase filter_buffer,
                            se::DeviceMemoryBase output_buffer,
                            const se::dnn::BatchDescriptor& input_desc,
                            const se::dnn::FilterDescriptor& filter_desc,
                            const se::dnn::BatchDescriptor& output_desc,
                            const se::dnn::ConvolutionDescriptor& conv_desc,
                            se::StreamExecutor* stream_exec,
                            absl::Span<const AutotuneResult> results) {
  LogConvAutotuneResultsImpl<AutotuneResult, AutotuningLog>(
      kind, element_type, input_buffer, filter_buffer, output_buffer,
      input_desc, filter_desc, output_desc, conv_desc, stream_exec, results);
}

void LogConvAutotuneResults(se::dnn::ConvolutionKind kind,
    se::dnn::DataType element_type, se::DeviceMemoryBase input_buffer,
    se::DeviceMemoryBase filter_buffer, se::DeviceMemoryBase output_buffer,
    const se::dnn::BatchDescriptor& input_desc,
    const se::dnn::FilterDescriptor& filter_desc,
    const se::dnn::BatchDescriptor& output_desc,
    const se::dnn::ConvolutionDescriptor& conv_desc,
    se::StreamExecutor* stream_exec,
    absl::Span<const AutotuneExecutionPlanResult> results) {
  LogConvAutotuneResultsImpl<AutotuneExecutionPlanResult,
                             AutotuningExecutionPlanLog>(
      kind, element_type, input_buffer, filter_buffer, output_buffer,
      input_desc, filter_desc, output_desc, conv_desc, stream_exec, results);
}

void LogFusedConvForwardAutotuneResults(
    se::dnn::DataType element_type, se::DeviceMemoryBase input_buffer,
    se::DeviceMemoryBase filter_buffer, se::DeviceMemoryBase output_buffer,
    se::DeviceMemoryBase bias_buffer, se::DeviceMemoryBase side_input_buffer,
    const se::dnn::BatchDescriptor& input_desc,
    const se::dnn::FilterDescriptor& filter_desc,
    const se::dnn::BatchDescriptor& output_desc,
    const se::dnn::ConvolutionDescriptor& conv_desc, double conv_scale,
    double side_value_scale, se::dnn::ActivationMode activation_mode,
    se::StreamExecutor* stream_exec, absl::Span<const AutotuneResult> results) {
  LogFusedConvForwardAutotuneResultsImpl<AutotuneResult, AutotuningLog>(
      element_type, input_buffer, filter_buffer, output_buffer, bias_buffer,
      side_input_buffer, input_desc, filter_desc, output_desc, conv_desc,
      conv_scale, side_value_scale, activation_mode, stream_exec, results);
}

void LogFusedConvForwardAutotuneResults(
    se::dnn::DataType element_type, se::DeviceMemoryBase input_buffer,
    se::DeviceMemoryBase filter_buffer, se::DeviceMemoryBase output_buffer,
    se::DeviceMemoryBase bias_buffer, se::DeviceMemoryBase side_input_buffer,
    const se::dnn::BatchDescriptor& input_desc,
    const se::dnn::FilterDescriptor& filter_desc,
    const se::dnn::BatchDescriptor& output_desc,
    const se::dnn::ConvolutionDescriptor& conv_desc, double conv_scale,
    double side_value_scale, se::dnn::ActivationMode activation_mode,
    se::StreamExecutor* stream_exec,
    absl::Span<const AutotuneExecutionPlanResult> results) {
  LogFusedConvForwardAutotuneResultsImpl<AutotuneExecutionPlanResult,
                                         AutotuningExecutionPlanLog>(
      element_type, input_buffer, filter_buffer, output_buffer, bias_buffer,
      side_input_buffer, input_desc, filter_desc, output_desc, conv_desc,
      conv_scale, side_value_scale, activation_mode, stream_exec, results);
}

Status BestCudnnConvAlgorithm(absl::Span<const AutotuneResult> results,
                              se::dnn::AlgorithmConfig* algo) {
  std::vector<AutotuneResult> filtered_results;
  absl::c_copy_if(
      results, std::back_inserter(filtered_results),
      [](const AutotuneResult& result) { return !result.has_failure(); });
  if (filtered_results.empty()) {
    return errors::NotFound("No algorithm worked!");
  }
  std::vector<AutotuneResult> filtered_results_no_scratch;
  absl::c_copy_if(
      filtered_results, std::back_inserter(filtered_results_no_scratch),
      [](const AutotuneResult& result) { return result.scratch_bytes() == 0; });

  auto selected_result = filtered_results.begin();
  auto selected_result_no_scratch = filtered_results_no_scratch.begin();
  if (!se::cuda::RequireCuDNNDeterminism()) {
    auto compare_run_times = [](const AutotuneResult& lhs,
                                const AutotuneResult& rhs) {
      return proto_utils::FromDurationProto(lhs.run_time()) <
             proto_utils::FromDurationProto(rhs.run_time());
    };
    selected_result = absl::c_min_element(filtered_results, compare_run_times);
    selected_result_no_scratch = absl::c_min_element(
        filtered_results_no_scratch, compare_run_times);
  }

  algo->set_algorithm({selected_result->conv().algorithm(),
                       selected_result->conv().tensor_ops_enabled()});
  if (selected_result_no_scratch != filtered_results_no_scratch.end()) {
    algo->set_algorithm_no_scratch(
        {selected_result_no_scratch->conv().algorithm(),
         selected_result_no_scratch->conv().tensor_ops_enabled()});
  }

  return Status::OK();
}

Status BestCudnnConvExecutionPlan(
           absl::Span<const AutotuneExecutionPlanResult> results,
           int* idx, int* idx_no_scratch) {
  *idx = -1;
  *idx_no_scratch = -1;
  for (int i = 0; i < results.size(); i++) {
    if (!results[i].has_failure()) {
      if (*idx == -1 or
          proto_utils::FromDurationProto(results[i].run_time()) <
          proto_utils::FromDurationProto(results[*idx].run_time())) {
        *idx = i;
      }
      if (results[i].scratch_bytes() == 0 and (*idx_no_scratch == -1 or
          proto_utils::FromDurationProto(results[i].run_time()) <
          proto_utils::FromDurationProto(
              results[*idx_no_scratch].run_time()))) {
        *idx_no_scratch = i;
      }
    }
  }

  if (*idx == -1 and *idx_no_scratch == -1) {
    return errors::NotFound("No execution plan worked!");
  }

  return Status::OK();
}

}  // namespace tensorflow

#endif  // GOOGLE_CUDA
