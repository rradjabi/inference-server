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

import os
import sys

import cv2
import numpy as np
import pytest

import amdinfer
import amdinfer.pre_post as pre_post
import amdinfer.testing

from helper import root_path, run_benchmark


def preprocess(paths):
    """
    Given a list of paths to images, preprocess the images and return them

    Args:
        paths (list[str]): Paths to images

    Returns:
        list[numpy.ndarray]: List of images
    """
    options = pre_post.ImagePreprocessOptionsFloat()
    options.order = pre_post.ImageOrder.NCHW
    options.mean = [0.485, 0.456, 0.406]
    options.std = [4.367, 4.464, 4.444]
    options.normalize = True
    options.convert_color = True
    options.color_code = cv2.COLOR_BGR2RGB
    options.convert_type = True
    options.type = cv2.CV_32FC3
    options.convert_scale = 1.0 / 255.0
    return pre_post.imagePreprocessFloat(paths, options)


def postprocess(output, k):
    """
    Postprocess the output data. For ResNet50, this includes performing a softmax
    to determine the most probable classifications

    Args:
        output (amdinfer.InferenceResponseOutput): the output from the inference server
        k (int): number of top categories to return

    Returns:
        list[int]: indices for the top k categories
    """
    return pre_post.resnet50PostprocessFloat(output, k)


@pytest.mark.extensions(["ptzendnn"])
@pytest.mark.usefixtures("load")
class TestPtZendnn:
    """
    Test the PtZendnn worker
    """

    @staticmethod
    def get_config():
        model = "PtZendnn"
        parameters = {
            "model": amdinfer.testing.getPathToAsset("pt_resnet50"),
            "input_size": 224,
            "output_classes": 1000,
            "batch_size": 8,
        }
        return (model, parameters)

    def send_request(self, request, check_asserts=True):
        """
        Sends the given request to the server and asserts common checks

        Args:
            request (InferenceRequest): request to send to the server
            output (np.ndarray): Output to check against golden output
            check_asserts (bool): Verify image against golden

        Returns:
            Response: Response as a dictionary
        """

        try:
            response = self.rest_client.modelInfer(self.endpoint, request)
        except ConnectionError:
            pytest.fail(
                "Connection to the amdinfer server ended without response!", False
            )

        num_inputs = len(request.getInputs())

        if check_asserts:
            assert not response.isError(), response.getError()
            assert response.id == ""
            assert response.model == "PTModel"
            outputs = response.getOutputs()
            assert len(outputs) == num_inputs
            for index, output in enumerate(outputs):
                assert output.name == "input" + str(index)
                assert output.datatype == amdinfer.DataType.FP32
                assert output.parameters.empty()
        return response

    @pytest.mark.parametrize("num", [1])
    def test_ptzendnn_0(self, num):
        """
        Send a request to pt model as tensor data
        """
        image_path = amdinfer.testing.getPathToAsset("asset_dog-3619020_640.jpg")

        batch = num
        image_paths = [image_path] * batch
        images = preprocess(image_paths)
        for image in images:
            request = amdinfer.ImageInferenceRequest(image, True)
            response = self.send_request(request)
            outputs = response.getOutputs()
            top_k_responses = postprocess(outputs[0], 5)
            gold_response_output = [259, 261, 260, 154, 157]
            assert top_k_responses == gold_response_output
