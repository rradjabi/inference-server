# Copyright 2021 Xilinx, Inc.
# Copyright 2022 Advanced Micro Devices, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import json

import pytest

import amdinfer
import amdinfer.testing

from helper import root_path


@pytest.mark.extensions(["aks", "vitis"])
@pytest.mark.fpgas("DPUCADF8H", 1)
@pytest.mark.usefixtures("load")
class TestResnet50Stream:
    """
    Test the Resnet50Stream
    """

    @staticmethod
    def get_config():
        model = "Resnet50Stream"
        parameters = None
        return (model, parameters)

    def construct_request(self, requested_frames_count):
        video_path = amdinfer.testing.getPathToAsset("asset_Physicsworks.ogv")

        input_0 = amdinfer.InferenceRequestInput()
        input_0.name = "input0"
        input_0.datatype = amdinfer.DataType.STRING
        input_0.setStringData(amdinfer.stringToArray(video_path))
        input_0.shape = [len(video_path)]
        parameters = amdinfer.ParameterMap()
        parameters.put("count", requested_frames_count)
        input_0.parameters = parameters

        request = amdinfer.InferenceRequest()
        request.addInputTensor(input_0)
        parameters_2 = amdinfer.ParameterMap()
        parameters_2.put("key", "0")
        request.parameters = parameters_2

        self.ws_client.modelInferWs(self.endpoint, request)
        response_str = self.ws_client.modelRecv()
        response = json.loads(response_str)

        assert response["key"] == "0"
        assert float(response["data"]["img"]) == 15.0

    def recv_frames(self, count):
        for i in range(count):
            resp_str = self.ws_client.modelRecv()
            resp = json.loads(resp_str)
            data = resp["data"]["img"]
            resp["data"]["img"].split(",")[1]

    def test_resnet50stream_0(self):
        requested_frames_count = 100
        self.construct_request(requested_frames_count)
        self.recv_frames(requested_frames_count)
        self.ws_client.close()

    # ? This is commented out because we need a function like run_benchmark()
    # ? to work with the pedantic mode to add the necessary metadata for the
    # ? benchmark to work with the benchmarking framework.
    # @pytest.mark.benchmark(group="resnet50stream")
    # def test_benchmark_resnet50stream_0(self, benchmark):
    #     requested_frames_count = 400
    #     self.construct_request(requested_frames_count)
    #     benchmark.pedantic(self.recv_frames, args=(requested_frames_count,), iterations=1, rounds=1)
