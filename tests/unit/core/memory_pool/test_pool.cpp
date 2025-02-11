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

#include <tuple>

#include "amdinfer/buffers/buffer.hpp"  // for BufferPtr
#include "amdinfer/core/exceptions.hpp"
#include "amdinfer/core/inference_request.hpp"  // for InferenceRequestInput
#include "amdinfer/core/memory_pool/pool.hpp"
#include "amdinfer/testing/gtest.hpp"  // for AssertionResult,...

namespace amdinfer {

// NOLINTNEXTLINE(cert-err58-cpp, cppcoreguidelines-owning-memory)
TEST(UnitPool, Basic) {
  MemoryPool pool;
  InferenceRequestInput input{nullptr, {1}, DataType::Int32};

  auto buffer = pool.get({MemoryAllocators::Cpu}, input, 1);

  pool.put(std::move(buffer));
}

}  // namespace amdinfer
