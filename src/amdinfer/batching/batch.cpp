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

#include "amdinfer/batching/batch.hpp"

#include <cassert>

#include "amdinfer/buffers/buffer.hpp"
#include "amdinfer/observation/tracing.hpp"

namespace amdinfer {

void Batch::addRequest(InferenceRequestPtr request) {
  requests_.push_back(std::move(request));
}

std::vector<Buffer*> Batch::getRawInputBuffers() const {
  std::vector<Buffer*> buffers;
  buffers.reserve(input_buffers_.size());
  for (const auto& buffer : input_buffers_) {
    buffers.push_back(buffer.get());
  }
  return buffers;
}

BufferPtrs Batch::getInputBuffers() { return std::move(input_buffers_); }

std::vector<Buffer*> Batch::getRawOutputBuffers() const {
  std::vector<Buffer*> buffers;
  buffers.reserve(output_buffers_.size());
  for (const auto& buffer : output_buffers_) {
    buffers.push_back(buffer.get());
  }
  return buffers;
}

BufferPtrs Batch::getOutputBuffers() { return std::move(output_buffers_); }

const std::vector<InferenceRequestPtr>& Batch::getRequests() const {
  return requests_;
}

const InferenceRequestPtr& Batch::getRequest(size_t index) {
  return requests_.at(index);
}

bool Batch::empty() const { return requests_.empty(); }

size_t Batch::size() const {
#ifdef AMDINFER_ENABLE_TRACING
  assert(requests_.size() == traces_.size());
#endif
#ifdef AMDINFER_ENABLE_METRICS
  assert(requests_.size() == start_times_.size());
#endif

  return requests_.size();
}

size_t Batch::getInputSize() const { return input_buffers_.size(); }

size_t Batch::getOutputSize() const { return output_buffers_.size(); }

void Batch::setBuffers(BufferPtrs inputs, BufferPtrs outputs) {
  input_buffers_ = std::move(inputs);
  output_buffers_ = std::move(outputs);
}

#ifdef AMDINFER_ENABLE_TRACING
void Batch::addTrace(TracePtr trace) { traces_.push_back(std::move(trace)); }

TracePtr& Batch::getTrace(size_t index) { return traces_.at(index); }
#endif

#ifdef AMDINFER_ENABLE_METRICS
void Batch::addTime(std::chrono::high_resolution_clock::time_point timestamp) {
  start_times_.push_back(timestamp);
}

std::chrono::high_resolution_clock::time_point Batch::getTime(size_t index) {
  return start_times_.at(index);
}
#endif

}  // namespace amdinfer
