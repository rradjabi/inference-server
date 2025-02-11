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
 * @brief Implements the VartTensorBuffer class
 */

#include "amdinfer/buffers/vart_tensor.hpp"

#include <vart/tensor_buffer.hpp>
#include <vector>                 // for vector
#include <xir/tensor/tensor.hpp>  // for Tensor

namespace amdinfer {

VartTensorBuffer::VartTensorBuffer(void* data, MemoryAllocators allocator)
  : Buffer(allocator), data_(static_cast<vart::TensorBuffer*>(data)) {}

void* VartTensorBuffer::data(size_t offset) {
  const auto* tensor = data_->get_tensor();

  // Some DPUs need a shape argument to data() to get the data properly.
  // This argument should be the same size as the tensor (by default,
  // [batch, h, w, c]). This first argument is the batch index. The other
  // indices should be zero to get the start of the batch
  std::vector<int> shape(tensor->get_shape().size(), 0);

  auto dims = tensor->get_shape();
  auto size = tensor->get_shape().size();

  // convert the offset to a shape based on the tensor shape
  for (auto k = 0U; k < size; k++) {
    auto stride = 1;
    for (auto m = k + 1; m < size; m++) {
      stride *= dims[m];
    }
    shape[k] = static_cast<int>(offset / stride);
    offset -= shape[k] * stride;
  }

  return reinterpret_cast<void*>(data_->data(shape).first);
}

vart::TensorBuffer* VartTensorBuffer::getTensorBuffer() { return data_; }

}  // namespace amdinfer
