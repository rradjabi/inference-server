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
 * @brief Implements helpful functions for working with containers in the
 * standard library.
 */

#include <iterator>
#include <numeric>
#include <type_traits>
#include <vector>

#ifndef GUARD_AMDINFER_UTIL_CONTAINERS
#define GUARD_AMDINFER_UTIL_CONTAINERS

namespace amdinfer::util {

template <typename Iter>
inline auto containerProduct(Iter begin, Iter end) {
  // get the value associated with the iterator and use that type to set the
  // initialization value
  using value_type = typename std::iterator_traits<Iter>::value_type;
  static_assert(std::is_arithmetic_v<value_type>,
                "The container must contain an arithmetic type");

  value_type init = 1;
  return std::accumulate(begin, end, init, std::multiplies<>());
}

template <typename Container>
inline auto containerProduct(const Container& c) {
  return containerProduct(std::begin(c), std::end(c));
}

template <typename Iter>
inline auto containerSum(Iter begin, Iter end) {
  // get the value associated with the iterator and use that type to set the
  // initialization value
  using value_type = typename std::iterator_traits<Iter>::value_type;
  static_assert(std::is_arithmetic_v<value_type>,
                "The container must contain an arithmetic type");

  value_type init = 0;
  return std::accumulate(begin, end, init, std::plus<>());
}

template <typename Container>
inline auto containerSum(const Container& c) {
  return containerSum(std::begin(c), std::end(c));
}

}  // namespace amdinfer::util

#endif  // GUARD_AMDINFER_UTIL_CONTAINERS
