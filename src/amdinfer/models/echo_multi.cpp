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
 * @brief Implements the echo_multi model
 */

#include "amdinfer/batching/batch.hpp"
#include "amdinfer/core/inference_request.hpp"
#include "amdinfer/core/inference_response.hpp"
#include "amdinfer/core/parameters.hpp"
#include "amdinfer/observation/logging.hpp"
#include "amdinfer/observation/metrics.hpp"
#include "amdinfer/observation/tracing.hpp"
#include "amdinfer/util/containers.hpp"
#include "amdinfer/util/memory.hpp"
#include "amdinfer/util/timer.hpp"

const int kInputTensors = 2;
const std::array<size_t, kInputTensors> kInputLengths = {1, 2};
const int kOutputTensors = 3;
const std::array<size_t, kOutputTensors> kOutputLengths = {1, 4, 3};

extern "C" {

std::vector<amdinfer::Tensor> getInputs() {
  std::vector<amdinfer::Tensor> input_tensors;
  input_tensors.reserve(kInputTensors);
  for (auto i = 0; i < kInputTensors; ++i) {
    std::vector<size_t> shape = {kInputLengths.at(i)};
    input_tensors.emplace_back("input" + std::to_string(i), shape,
                               amdinfer::DataType::Uint32);
  }
  return input_tensors;
}

std::vector<amdinfer::Tensor> getOutputs() {
  std::vector<amdinfer::Tensor> output_tensors;
  output_tensors.reserve(kOutputTensors);
  for (auto i = 0; i < kOutputTensors; ++i) {
    std::vector<size_t> shape = {kOutputLengths.at(i)};
    output_tensors.emplace_back("output" + std::to_string(i), shape,
                                amdinfer::DataType::Uint32);
  }
  return output_tensors;
}

void run(amdinfer::Batch* batch, amdinfer::Batch* new_batch) {
  amdinfer::Logger logger{amdinfer::Loggers::Server};

  const auto batch_size = batch->size();
  for (unsigned int j = 0; j < batch_size; j++) {
    const auto& req = batch->getRequest(j);
#ifdef AMDINFER_ENABLE_TRACING
    auto& trace = batch->getTrace(j);
    trace->startSpan("echoMulti");
#endif
    const auto& new_request = new_batch->getRequest(j);
    new_request->setCallback(req->getCallback());

    // new_request->setID(req->getID());
    const auto& inputs = req->getInputs();
    // auto outputs = req->getOutputs();

    std::vector<int> args;
    const auto input_num = amdinfer::util::containerSum(kInputLengths);
    assert(input_num != 0);
    args.reserve(input_num);
    for (auto i = 0; i < kInputTensors; ++i) {
      const auto* input_buffer = static_cast<uint32_t*>(inputs[i].getData());
      const auto len = kInputLengths.at(i);
      for (auto k = 0U; k < len; ++k) {
        args.push_back(input_buffer[k]);
      }
    }

    const auto& new_inputs = new_request->getInputs();
    auto input_index = 0;
    for (auto i = 0; i < kOutputTensors; ++i) {
      const auto len = kOutputLengths.at(i);
      auto* data = static_cast<std::byte*>(new_inputs.at(i).getData());
      for (auto k = 0U; k < len; ++k) {
        data = amdinfer::util::copy(args[input_index], data);
        input_index = (input_index + 1) % input_num;
      }
    }

#ifdef AMDINFER_ENABLE_TRACING
    trace->endSpan();
    new_batch->addTrace(std::move(trace));
#endif

#ifdef AMDINFER_ENABLE_METRICS
    new_batch->addTime(batch->getTime(j));
#endif
  }
}

}  // extern "C"
