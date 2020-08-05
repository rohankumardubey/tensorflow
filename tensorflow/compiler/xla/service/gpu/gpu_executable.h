/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_EXECUTABLE_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_EXECUTABLE_H_

#include <memory>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/xla/service/buffer_assignment.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/gpu/buffer_allocations.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_types.h"
#include "tensorflow/compiler/xla/service/gpu/stream_assignment.h"
#include "tensorflow/compiler/xla/service/gpu/thunk.h"
#include "tensorflow/compiler/xla/service/gpu/thunk_schedule.h"
#include "tensorflow/compiler/xla/service/hlo_dataflow_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_execution_profile.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"
#include "tensorflow/stream_executor/device_memory_allocator.h"
#include "tensorflow/stream_executor/gpu/gpu_driver.h"

namespace xla {
namespace gpu {

// GPU-targeting implementation of the XLA Executable interface.
//
// Launches the given GPU kernel via the StreamExecutor.
//
// This is an immutable data type after initialization, and thus thread safe.
class GpuExecutable : public Executable {
 public:
  // We need to share ownership of hlo_module and assignment with profiler to
  // safely keep a reference to these objects during tracing period, thus they
  // are passed as shared pointers.
  GpuExecutable(const string& text, const std::vector<uint8>& binary,
                GpuVersion gpu_version,
                std::unique_ptr<const ThunkSchedule> thunk_schedule,
                std::shared_ptr<HloModule> hlo_module,
                std::shared_ptr<const BufferAssignment> assignment,
                std::unique_ptr<HloProfilePrinterData> hlo_profile_printer_data,
                std::unique_ptr<HloProfileIndexMap> hlo_profile_index_map);
  ~GpuExecutable() override;

  int64 SizeOfGeneratedCodeInBytes() override;

  // This should be called after set_ir_module_string.
  const string& ir_module_string() const { return ir_module_string_; }

  // This should be called before ExecuteOnStream.
  void set_ir_module_string(const string& ir_module_string) {
    ir_module_string_ = ir_module_string;
  }

  // Returns the compiled code for the computation. The compiled code is PTX in
  // Cuda and unused empty string in ROCm.
  const string& text() const { return text_; }

  // Returns the binary stored in this GpuExecutable. The binary is cubin in
  // Cuda, and HSA code object in ROCm. It may be empty, in which case
  // compilation is left up to the GPU driver.
  const std::vector<uint8>& binary() const { return binary_; }

  // ExecuteAsyncOnStream will fail if the compute capability of the stream
  // doesn't match the compute capability passed to this object's constructor.
  StatusOr<ExecutionOutput> ExecuteAsyncOnStream(
      const ServiceExecutableRunOptions* run_options,
      std::vector<ShapeTree<MaybeOwningDeviceMemory>> arguments,
      HloExecutionProfile* hlo_execution_profile) override;

  std::shared_ptr<const BufferAssignment> GetBufferAssignment() const {
    return assignment_;
  }

 private:
  // If `block_host_until_done` is false, execution will not block the host
  // until the kernels have completed. This is used as an optimization for
  // clients, such as Tensorflow, that use a single stream of execution for
  // computations, and allow host-side deallocation from the allocator before
  // GPU execution completes.
  Status ExecuteThunks(const ServiceExecutableRunOptions* run_options,
                       const BufferAllocations& buffer_allocations,
                       bool block_host_until_done,
                       HloExecutionProfile* hlo_execution_profile);

  // Returns the value set of the root instruction of the entry
  // computation. Uses dataflow analysis from buffer assignment.
  const InstructionValueSet& GetRootValueSet() const;

  using BufferAllocToDeviceMemoryMap =
      absl::flat_hash_map<BufferAllocation::Index, se::DeviceMemoryBase>;

  // Loads the PTX or CUBIN for this executable into `executor` and resolves the
  // globals corresponding to constant buffers.  Returns a map mapping buffer
  // allocation indices to GPU pointers.
  StatusOr<const BufferAllocToDeviceMemoryMap*> ResolveConstantGlobals(
      stream_executor::Stream* stream);

  // Computes annotations for each thunk and store them in thunk_annotations_.
  void ComputeThunkAnnotations();

  // GpuExecutable check with either AMD's ISA version, or Nvidia's major minor
  // version for compute capability, depending on the hardware.
  Status CheckCompatibilityWithServiceExecutableRunOptions(
      const ServiceExecutableRunOptions* run_options);

  // Returns whether GPU graph capture can safely be used for execution of
  // this executable.
  bool CanUseGpuGraphCapture() const;

  // The LLVM IR, in string format, of the unoptimized module generated for this
  // GpuExecutable. We save a string instead of an llvm::Module* because leaving
  // llvm::Module* in a singleton can cause the heap checker to emit false
  // positives.
  //
  // This string should be modified only before ExecuteOnStream.
  string ir_module_string_;

  // The compiled code for the computation.
  const string text_;

  // The GPU machine code for the computation, targeting GPUs at
  // compute_capability_.
  //
  // May be empty, in which case we leave compilation up to the GPU driver.
  const std::vector<uint8> binary_;

  // The GPU version for compute compatibility check.
  GpuVersion gpu_version_;

  // The thunks to be invoked by this GpuExecutable. They are generated by the
  // IrEmitter.
  const std::unique_ptr<const ThunkSchedule> thunk_schedule_;

  // Owns the buffer data at runtime. It provides information to allocate
  // memory for every output/temp buffers.
  const std::shared_ptr<const BufferAssignment> assignment_;

  // Maps a thunk to a string describing the thunk.  This is useful when
  // constructing ScopeAnnotation objects.
  absl::flat_hash_map<Thunk*, string> thunk_annotations_;

  // Cache of module handles and constant buffer allocation maps used by
  // `ResolveConstantGlobals`.
  tensorflow::mutex module_handle_mutex_;
  std::map<stream_executor::StreamExecutor*, se::ScopedModuleHandle>
      module_handles_ TF_GUARDED_BY(module_handle_mutex_);
  std::map<stream_executor::StreamExecutor*, BufferAllocToDeviceMemoryMap>
      module_globals_ TF_GUARDED_BY(module_handle_mutex_);

  bool can_use_gpu_graph_capture_ = false;

  se::internal::StreamExecutorInterface* executor_impl_ = nullptr;

  void SetExecutor(se::internal::StreamExecutorInterface* executor_impl) {
    executor_impl_ = executor_impl;
  }

  se::internal::StreamExecutorInterface* GetExecutor() {
    return executor_impl_;
  }
  /*
  // Maps GpuContext* to owned GpuGraphExec objects.
  // std::unordered_map<void*, void*> gpu_exec_graphs_;
  // Maps GpuContext* to allocation key to owned GpuGraphExec objects.
  std::unordered_map<void*,
                      std::unordered_map<BufferAllocations::KeyType, void*>>
       gpu_exec_graphs_ GUARDED_BY(graph_mutex_);*/

  struct MutexedGraphExecCache {
    tensorflow::mutex exec_graph_cache_mu_;
    int64 cache_size_ GUARDED_BY(exec_graph_cache_mu) = 0;
    stream_executor::gpu::GpuContext* GUARDED_BY(exec_graph_cache_mu)
        gpu_context_ = nullptr;
    std::list<void*> gpu_exec_graphs_ GUARDED_BY(exec_graph_cache_mu);
    std::unordered_map<BufferAllocations::KeyType, std::list<void*>::iterator>
        gpu_key_to_exec_graphs_map_ GUARDED_BY(exec_graph_cache_mu);

    // Pushing in a new pair of key and exec graph.
    void update_cache(BufferAllocations::KeyType key, void* gpu_exec_graph) {
      tensorflow::mutex_lock lock(exec_graph_cache_mu_);
      gpu_exec_graphs_.push_front(gpu_exec_graph);
      if (gpu_exec_graphs_.size() > cache_size_) {
        auto& graph_exec = gpu_exec_graphs_.back();
        auto* exec_graph =
            reinterpret_cast<stream_executor::gpu::GpuGraphExecHandle*>(
                &gpu_exec_graphs_.back());
        using stream_executor::gpu::GpuDriver;
        GpuDriver::DestroyExecutableGraph(gpu_context_, exec_graph);
        gpu_exec_graphs_.pop_back();
      }
      gpu_key_to_exec_graphs_map_[key] = gpu_exec_graphs_.begin();
    }

    void* get_exec_graph(BufferAllocations::KeyType key) {
      tensorflow::mutex_lock lock(exec_graph_cache_mu_);
      if (gpu_key_to_exec_graphs_map_.find(key) !=
          gpu_key_to_exec_graphs_map_.end()) {
        auto it = std::find(gpu_exec_graphs_.begin(), gpu_exec_graphs_.end(),
                            *(gpu_key_to_exec_graphs_map_[key]));
        if (it == gpu_exec_graphs_.end()) {
          gpu_key_to_exec_graphs_map_.erase(key);
          return nullptr;
        }
        auto gpu_exec_graph = *(gpu_key_to_exec_graphs_map_[key]);
        gpu_exec_graphs_.remove(gpu_exec_graph);
        gpu_exec_graphs_.push_front(gpu_exec_graph);
        gpu_key_to_exec_graphs_map_[key] = gpu_exec_graphs_.begin();
        return gpu_exec_graph;
      }
      return nullptr;
    }

    void set_cache_size(int64 cache_size) {
      tensorflow::mutex_lock lock(exec_graph_cache_mu_);
      cache_size_ = cache_size;
    }

    void set_gpu_context(stream_executor::gpu::GpuContext* gpu_context) {
      tensorflow::mutex_lock lock(exec_graph_cache_mu_);
      gpu_context_ = gpu_context;
    }

    size_t get_current_cache_size() {
      tensorflow::mutex_lock lock(exec_graph_cache_mu_);
      return gpu_exec_graphs_.size();
    }
  };

  struct MutexedGraphCacheStats {
    tensorflow::mutex graph_cache_mu;
    uint64 cache_hits GUARDED_BY(graph_cache_mu) = 0;
    uint64 cache_miss GUARDED_BY(graph_cache_mu) = 0;
    uint64 times_called GUARDED_BY(graph_cache_mu) = 0;
    size_t last_buf_key_hash GUARDED_BY(graph_cache_mu) = 0;
    uint64 last_buf_key_hits GUARDED_BY(graph_cache_mu) = 0;
  };

  MutexedGraphCacheStats graph_stats_;

  std::unordered_map<void*, MutexedGraphExecCache> gpu_exec_graphs_cache_
      GUARDED_BY(module_handle_mutex_);

  TF_DISALLOW_COPY_AND_ASSIGN(GpuExecutable);
};

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_EXECUTABLE_H_
