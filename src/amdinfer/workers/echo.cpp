// Copyright 2021 Xilinx, Inc.
// Copyright 2022 Advanced Micro Devices, Inc.
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
 * @brief Implements the Echo worker
 */

#include <cstddef>    // for size_t, byte
#include <cstdint>    // for uint32_t, int32_t
#include <cstring>    // for memcpy
#include <exception>  // for exception
#include <memory>     // for unique_ptr, allocator
#include <ratio>      // for micro
#include <string>     // for string
#include <thread>     // for thread
#include <utility>    // for move
#include <vector>     // for vector

#include "amdinfer/batching/hard.hpp"    // for HardBatcher
#include "amdinfer/build_options.hpp"    // for AMDINFER_ENABLE_TRACING
#include "amdinfer/core/data_types.hpp"  // for DataType, DataType::Uint32
#include "amdinfer/core/inference_request.hpp"   // for InferenceRequest
#include "amdinfer/core/inference_response.hpp"  // for InferenceResponse
#include "amdinfer/core/parameters.hpp"          // for ParameterMap
#include "amdinfer/declarations.hpp"         // for BufferPtr, InferenceRes...
#include "amdinfer/observation/logging.hpp"  // for Logger
#include "amdinfer/observation/metrics.hpp"  // for Metrics
#include "amdinfer/observation/tracing.hpp"  // for startFollowSpan, SpanPtr
#include "amdinfer/util/thread.hpp"          // for setThreadName
#include "amdinfer/util/timer.hpp"           // for Timer
#include "amdinfer/workers/worker.hpp"       // for Worker

namespace amdinfer::workers {

/**
 * @brief The Echo worker is a simple worker that accepts a single uint32_t
 * argument and adds 1 to it and returns. It accepts multiple input tensors and
 * returns the corresponding number of output tensors.
 *
 */
class Echo : public Worker {
 public:
  using Worker::Worker;
  std::thread spawn(BatchPtrQueue* input_queue) override;
  [[nodiscard]] std::vector<MemoryAllocators> getAllocators() const override;

 private:
  void doInit(ParameterMap* parameters) override;
  void doAcquire(ParameterMap* parameters) override;
  void doRun(BatchPtrQueue* input_queue) override;
  void doRelease() override;
  void doDestroy() override;

  // workers define what batcher implementation should be used for them.
  // if not explicitly defined here, a default value is used from worker.hpp.
  using Worker::makeBatcher;
  std::vector<std::unique_ptr<Batcher>> makeBatcher(int num,
                                                    ParameterMap* parameters,
                                                    MemoryPool* pool) override {
    return this->makeBatcher<HardBatcher>(num, parameters, pool);
  };
};

std::thread Echo::spawn(BatchPtrQueue* input_queue) {
  return std::thread(&Echo::run, this, input_queue);
}

std::vector<MemoryAllocators> Echo::getAllocators() const {
  return {MemoryAllocators::Cpu};
}

void Echo::doInit(ParameterMap* parameters) {
  constexpr auto kBatchSize = 1;

  auto batch_size = kBatchSize;
  if (parameters->has("batch_size")) {
    batch_size = parameters->get<int32_t>("batch_size");
  }
  this->batch_size_ = batch_size;
}

void Echo::doAcquire(ParameterMap* parameters) {
  (void)parameters;  // suppress unused variable warning

  this->metadata_.addInputTensor("input", {1}, DataType::Uint32);
  this->metadata_.addOutputTensor("output", {1}, DataType::Uint32);
}

void Echo::doRun(BatchPtrQueue* input_queue) {
  util::setThreadName("Echo");
#ifdef AMDINFER_ENABLE_LOGGING
  const auto& logger = this->getLogger();
#endif

  while (true) {
    BatchPtr batch;
    input_queue->wait_dequeue(batch);
    if (batch == nullptr) {
      break;
    }
    AMDINFER_LOG_INFO(logger, "Got request in echo");
#ifdef AMDINFER_ENABLE_METRICS
    Metrics::getInstance().incrementCounter(
      MetricCounterIDs::PipelineIngressWorker);
#endif
    for (unsigned int j = 0; j < batch->size(); j++) {
      const auto& req = batch->getRequest(j);
#ifdef AMDINFER_ENABLE_TRACING
      const auto& trace = batch->getTrace(j);
      trace->startSpan("echo");
#endif
      InferenceResponse resp;
      resp.setID(req->getID());
      resp.setModel("echo");
      auto inputs = req->getInputs();
      auto outputs = req->getOutputs();
      for (unsigned int i = 0; i < inputs.size(); i++) {
        auto* input_buffer = inputs[i].getData();
        // std::byte* output_buffer = outputs[i].getData();
        // auto* input_buffer = dynamic_cast<VectorBuffer*>(input_ptr);
        // auto* output_buffer = dynamic_cast<VectorBuffer*>(output_ptr);

        uint32_t value = *static_cast<uint32_t*>(input_buffer);

        // this is my operation: add one to the read argument. While this can't
        // raise an exception, if exceptions can happen, they should be handled
        try {
          value++;
        } catch (const std::exception& e) {
          AMDINFER_LOG_ERROR(logger, e.what());
          req->runCallbackError("Something went wrong");
          continue;
        }

        // output_buffer->write(value);

        InferenceResponseOutput output;
        output.setDatatype(DataType::Uint32);
        std::string output_name;
        if (i < outputs.size()) {
          output_name = outputs[i].getName();
        }

        if (output_name.empty()) {
          output.setName(inputs[0].getName());
        } else {
          output.setName(output_name);
        }
        output.setShape({1});
        std::vector<std::byte> buffer;
        buffer.resize(sizeof(uint32_t));
        memcpy(buffer.data(), &value, sizeof(uint32_t));
        output.setData(std::move(buffer));
        resp.addOutput(output);
      }

#ifdef AMDINFER_ENABLE_TRACING
      auto context = trace->propagate();
      resp.setContext(std::move(context));
#endif

      // respond back to the client
      req->runCallbackOnce(resp);
#ifdef AMDINFER_ENABLE_METRICS
      Metrics::getInstance().incrementCounter(
        MetricCounterIDs::PipelineEgressWorker);
      util::Timer timer{batch->getTime(j)};
      timer.stop();
      auto duration = timer.count<std::micro>();
      Metrics::getInstance().observeSummary(MetricSummaryIDs::RequestLatency,
                                            duration);
#endif
    }
    this->returnInputBuffers(std::move(batch));
  }
  AMDINFER_LOG_INFO(logger, "Echo ending");
}

void Echo::doRelease() {}
void Echo::doDestroy() {}

}  // namespace amdinfer::workers

extern "C" {
// using smart pointer here may cause problems inside shared object so managing
// manually
amdinfer::workers::Worker* getWorker() {
  return new amdinfer::workers::Echo("echo", "cpu");
}
}  // extern C
