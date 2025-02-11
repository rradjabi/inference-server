// Copyright 2022 Xilinx, Inc.
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
 * @brief Implements the Python bindings for the http.hpp header
 */

#include "amdinfer/clients/client.hpp"

#include <pybind11/cast.h>      // for arg
#include <pybind11/pybind11.h>  // for module_, sequence, class_, pybind11
#include <pybind11/stl.h>       // IWYU pragma: keep

#include "amdinfer/core/inference_request.hpp"   // for InferenceRequest
#include "amdinfer/core/inference_response.hpp"  // for InferenceResponse

namespace py = pybind11;

namespace amdinfer {

void wrapClient(py::module_ &m) {
  py::class_<Client> client{m, "Client"};

  m.def("serverHasExtension", &serverHasExtension, py::arg("client"),
        py::arg("extension"));
  m.def("waitUntilServerReady", &waitUntilServerReady, py::arg("client"));
  m.def("waitUntilModelReady", &waitUntilModelReady, py::arg("client"),
        py::arg("model"));

  m.def("inferAsyncOrdered", &inferAsyncOrdered, py::arg("client"),
        py::arg("model"), py::arg("requests"));
  m.def("inferAsyncOrderedBatched", &inferAsyncOrderedBatched,
        py::arg("client"), py::arg("model"), py::arg("requests"),
        py::arg("batch_sizes"));
}

}  // namespace amdinfer
