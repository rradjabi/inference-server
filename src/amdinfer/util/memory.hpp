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
 * @brief Defines helper functions associated with managing memory
 */

#ifndef GUARD_AMDINFER_UTIL_MEMORY
#define GUARD_AMDINFER_UTIL_MEMORY

#include <cassert>  // for assert
#include <cstddef>  // for byte
#include <cstring>  // for memcpy

namespace amdinfer::util {

template <typename T>
std::byte *copy(const T &src, std::byte *dst,
                [[maybe_unused]] size_t count = 0) {
  if constexpr (std::is_pointer_v<T>) {
    assert(count > 0);
    std::memcpy(dst, src, count);
    return dst + count;
  }
  std::memcpy(dst, &src, sizeof(T));
  return dst + sizeof(T);
}

}  // namespace amdinfer::util

#endif  // GUARD_AMDINFER_UTIL_MEMORY
