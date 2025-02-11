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

#include <array>    // for array
#include <cstdlib>  // for getenv, abs

#include "amdinfer/testing/get_path_to_asset.hpp"  // for getPathToAsset
#include "facedetect.hpp"                          // IWYU pragma: associated
#include "gtest/gtest.h"  // for Test, AssertionResult, EXPECT_EQ

const std::array kGoldResponseOutput{
  -1.0F, 0.9937100410461426F, 268.0F, 78.728F, 158.0F, 170.800F,
};
const auto kGoldResponseSize = kGoldResponseOutput.size();

std::string prepareDirectory() {
  fs::path temp_dir =
    fs::temp_directory_path() / "amdinfer/tests/cpp/native/facedetect";
  fs::create_directories(temp_dir);
  const auto src_file =
    fs::path(amdinfer::getPathToAsset("asset_girl-1867092_640.jpg"));
  fs::copy_file(src_file, temp_dir / src_file.filename(),
                fs::copy_options::skip_existing);
  return temp_dir;
}

void dequeueValidate(FutureQueue& my_queue, int num_images) {
  std::future<amdinfer::InferenceResponse> element;
  for (int i = 0; i < num_images; i++) {
    my_queue.wait_dequeue(element);
    auto results = element.get();

    EXPECT_STREQ(results.getID().c_str(), "");
    EXPECT_STREQ(results.getModel().c_str(), "facedetect");
    auto outputs = results.getOutputs();
    EXPECT_EQ(outputs.size(), 1);
    for (auto& output : outputs) {
      const auto* data = static_cast<float*>(output.getData());
      auto size = output.getSize();
      EXPECT_STREQ(output.getName().c_str(), "");
      EXPECT_STREQ(output.getDatatype().str(), "FP32");
      const auto& parameters = output.getParameters();
      EXPECT_TRUE(parameters.empty());
      auto num_boxes = 1;
      auto shape = output.getShape();
      EXPECT_EQ(shape.size(), 2);
      EXPECT_EQ(shape[0], kGoldResponseSize);
      EXPECT_EQ(shape[1], num_boxes);
      EXPECT_EQ(size, kGoldResponseSize);
      const auto tolerance = 0.05;
      for (size_t j = 0; j < kGoldResponseSize; j++) {
        // expect that the response values are within 5% of the golden
        const auto abs_error = std::abs(kGoldResponseOutput.at(j) * tolerance);
        EXPECT_NEAR(data[j], kGoldResponseOutput.at(j), abs_error);
      }
    }
  }
}

// @pytest.mark.extensions(["vitis"])
// @pytest.mark.fpgas("DPUCADF8H", 1)
// NOLINTNEXTLINE(cert-err58-cpp, cppcoreguidelines-owning-memory)
TEST(Native, Facedetect) {
  amdinfer::Server server;
  amdinfer::NativeClient client(&server);

  auto fpgas_exist = client.hasHardware("DPUCADF8H", 1);
  if (!fpgas_exist) {
    GTEST_SKIP() << "No FPGAs available";
  }

  auto path = prepareDirectory();
  auto worker_name = load(client, 1);
  auto image_paths = getImages(path);
  auto num_images = image_paths.size();

  FutureQueue my_queue;
  run(client, image_paths, 1, worker_name, my_queue);

  dequeueValidate(my_queue, num_images);
}
