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
 * @brief Implements the InvertVideo worker
 */

#include <cstddef>                // for size_t
#include <cstdint>                // for int32_t
#include <memory>                 // for allocator, unique_ptr
#include <opencv2/core.hpp>       // for bitwise_not, Mat
#include <opencv2/imgcodecs.hpp>  // for imencode
#include <opencv2/videoio.hpp>    // for VideoCapture, CV_CAP_PRO...
#include <string>                 // for string, operator+, char_...
#include <thread>                 // for thread
#include <vector>                 // for vector

#include "amdinfer/batching/batcher.hpp"  // for Batch, BatchPtrQueue
#include "amdinfer/build_options.hpp"     // for AMDINFER_ENABLE_TRACING
#include "amdinfer/core/data_types.hpp"   // for DataType, DataType::String
#include "amdinfer/core/inference_request.hpp"   // for InferenceRequest
#include "amdinfer/core/inference_response.hpp"  // for InferenceResponse
#include "amdinfer/core/parameters.hpp"          // for ParameterMap
#include "amdinfer/declarations.hpp"         // for BufferPtr, InferenceRes...
#include "amdinfer/observation/logging.hpp"  // for Logger
#include "amdinfer/observation/tracing.hpp"  // for startFollowSpan, SpanPtr
#include "amdinfer/util/base64.hpp"          // for base64_encode
#include "amdinfer/util/thread.hpp"          // for setThreadName
#include "amdinfer/workers/worker.hpp"       // for Worker

namespace amdinfer {

std::string constructMessage(const std::string& key, const std::string& data) {
  const std::string labels = R"([])";
  return R"({"key": ")" + key + R"(", "data": {"img": ")" + data +
         R"(", "labels": )" + labels + "}}";
}

namespace workers {

/**
 * @brief The InvertVideo worker is a simple worker that accepts an path to a
 * video and sends the inverted frames back to the client over a websocket.
 *
 */
class InvertVideo : public Worker {
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
};

std::thread InvertVideo::spawn(BatchPtrQueue* input_queue) {
  return std::thread(&InvertVideo::run, this, input_queue);
}

std::vector<MemoryAllocators> InvertVideo::getAllocators() const {
  return {MemoryAllocators::Cpu};
}

void InvertVideo::doInit([[maybe_unused]] ParameterMap* parameters) {
  constexpr auto kBatchSize = 1;

  this->batch_size_ = kBatchSize;
}

// Support up to Full HD
const auto kMaxImageHeight = 1080;
const auto kMaxImageWidth = 1920;
const auto kMaxImageChannels = 3;

// arbitrarily choose max URL length for the video
const auto kMaxUrlLength = 128;

void InvertVideo::doAcquire(ParameterMap* parameters) {
  (void)parameters;  // suppress unused variable warning

  this->metadata_.addInputTensor("input", {kMaxUrlLength}, DataType::String);
  // TODO(varunsh): output is variable
  this->metadata_.addOutputTensor(
    "output", {kMaxImageHeight, kMaxImageWidth, kMaxImageChannels},
    DataType::Int8);
}

void InvertVideo::doRun(BatchPtrQueue* input_queue) {
  util::setThreadName("InvertVideo");
#ifdef AMDINFER_ENABLE_LOGGING
  const auto& logger = this->getLogger();
#endif

  while (true) {
    BatchPtr batch;
    input_queue->wait_dequeue(batch);
    if (batch == nullptr) {
      break;
    }

    AMDINFER_LOG_INFO(logger, "Got request in InvertVideo");
    for (unsigned int j = 0; j < batch->size(); j++) {
      const auto& req = batch->getRequest(j);
#ifdef AMDINFER_ENABLE_TRACING
      const auto& trace = batch->getTrace(j);
      trace->startSpan("InvertVideo");
#endif
      auto inputs = req->getInputs();
      auto outputs = req->getOutputs();
      auto key = req->getParameters().get<std::string>("key");
      for (auto& input : inputs) {
        auto* input_buffer = input.getData();

        auto* idata = static_cast<char*>(input_buffer);

        cv::VideoCapture cap(idata);  // open the video file
        if (!cap.isOpened()) {        // check if we succeeded
          const char* error = "Cannot open video file";
          AMDINFER_LOG_ERROR(logger, error);
          req->runCallbackError(error);
          continue;
        }

        InferenceResponse resp;
        resp.setID(req->getID());
        resp.setModel("invert_video");

        // contains the number of frames in the video;
        auto count = static_cast<int32_t>(
          cap.get(cv::VideoCaptureProperties::CAP_PROP_FRAME_COUNT));
        if (input.getParameters().has("count")) {
          count = input.getParameters().get<int32_t>("count");
        }
        double fps = cap.get(cv::VideoCaptureProperties::CAP_PROP_FPS);

        InferenceResponseOutput output;
        output.setName("key");
        output.setDatatype(DataType::String);
        auto message = constructMessage(key, std::to_string(fps));
        std::vector<std::byte> buffer;
        buffer.resize(message.size());
        memcpy(buffer.data(), message.data(), message.size());
        output.setData(std::move(buffer));
        output.setShape({message.size()});
        resp.addOutput(output);
        req->runCallback(resp);
        for (int num_frames = 0; num_frames < count; num_frames++) {
          cv::Mat frame;
          cap >> frame;  // get the next frame from video
          if (frame.empty()) {
            num_frames--;
            continue;
          }
          cv::bitwise_not(frame, frame);
          std::vector<unsigned char> buf;
          cv::imencode(".jpg", frame, buf);
          const auto* enc_msg = reinterpret_cast<const char*>(buf.data());
          std::string encoded =
            "data:image/jpg;base64," + util::base64Encode(enc_msg, buf.size());

          InferenceResponse resp;
          resp.setID(req->getID());
          resp.setModel("invert_video");

          InferenceResponseOutput output;
          output.setName("image");
          output.setDatatype(DataType::String);
          message = constructMessage(key, encoded);
          buffer.resize(message.size());
          memcpy(buffer.data(), message.data(), message.size());
          output.setData(std::move(buffer));
          output.setShape({message.size()});
          resp.addOutput(output);
          req->runCallback(resp);
        }
      }
    }
  }
  AMDINFER_LOG_INFO(logger, "InvertVideo ending");
}

void InvertVideo::doRelease() {}
void InvertVideo::doDestroy() {}

}  // namespace workers

}  // namespace amdinfer

extern "C" {
// using smart pointer here may cause problems inside shared object so managing
// manually
amdinfer::workers::Worker* getWorker() {
  return new amdinfer::workers::InvertVideo("InvertVideo", "CPU");
}
}  // extern C
