// Copyright 2023 Advanced Micro Devices, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file
 * @brief
 */

#ifndef GUARD_AMDINFER_BATCHING_BATCH
#define GUARD_AMDINFER_BATCHING_BATCH

#include "amdinfer/build_options.hpp"
#include "amdinfer/declarations.hpp"

namespace amdinfer {

class WorkerInfo;

/**
 * @brief The Batch is what the batcher produces and pushes to the workers. It
 * represents the requests, the buffers associated with the request and other
 * metadata that should be sent to the worker.
 *
 */
class Batch {
 public:
  void addRequest(InferenceRequestPtr request);

  void setBuffers(BufferPtrs inputs, BufferPtrs outputs);
  [[nodiscard]] const InferenceRequestPtr& getRequest(size_t index);
  [[nodiscard]] const std::vector<InferenceRequestPtr>& getRequests() const;
  [[nodiscard]] std::vector<BufferPtr> getInputBuffers();
  [[nodiscard]] std::vector<BufferPtr> getOutputBuffers();
  [[nodiscard]] std::vector<Buffer*> getRawInputBuffers() const;
  [[nodiscard]] std::vector<Buffer*> getRawOutputBuffers() const;

  [[nodiscard]] bool empty() const;
  [[nodiscard]] size_t size() const;
  [[nodiscard]] size_t getInputSize() const;
  [[nodiscard]] size_t getOutputSize() const;

#ifdef AMDINFER_ENABLE_TRACING
  void addTrace(TracePtr trace);
  TracePtr& getTrace(size_t index);
#endif
#ifdef AMDINFER_ENABLE_METRICS
  void addTime(std::chrono::high_resolution_clock::time_point timestamp);
  std::chrono::high_resolution_clock::time_point getTime(size_t index);
#endif

  [[nodiscard]] auto begin() const { return requests_.begin(); }
  [[nodiscard]] auto end() const { return requests_.end(); }

 private:
  const WorkerInfo* worker_;
  std::vector<InferenceRequestPtr> requests_;
  std::vector<BufferPtr> input_buffers_;
  std::vector<BufferPtr> output_buffers_;
#ifdef AMDINFER_ENABLE_TRACING
  std::vector<TracePtr> traces_;
#endif
#ifdef AMDINFER_ENABLE_METRICS
  std::vector<std::chrono::high_resolution_clock::time_point> start_times_;
#endif
};

using BatchPtr = std::unique_ptr<Batch>;

}  // namespace amdinfer

#endif  // GUARD_AMDINFER_BATCHING_BATCH
