"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
import unittest

import numpy as np
import oneflow as flow


@unittest.skipIf(
    not flow.unittest.env.eager_execution_enabled(),
    ".numpy() doesn't work in lazy mode",
)
class TestSlice(flow.unittest.TestCase):
    def test_slice(test_case):
        x = np.random.randn(3, 6, 9).astype(np.float32)
        input = flow.Tensor(x)
        tup_list = [[None, None, None], [0, 5, 2], [0, 6, 3]]
        y = flow.tmp.slice(input, slice_tup_list=tup_list)
        test_case.assertTrue(y.shape == flow.Size([3, 3, 2]))

    def test_tensor_slice(test_case):
        x = np.random.randn(2, 3, 4, 5).astype(np.float32)
        input = flow.Tensor(x)
        test_case.assertTrue(np.allclose(input[0].numpy(), x[0], 1e-5, 1e-5))
        test_case.assertTrue(np.allclose(input[1].numpy(), x[1], 1e-5, 1e-5))
        test_case.assertTrue(np.allclose(input[0, :].numpy(), x[0, :], 1e-5, 1e-5))
        test_case.assertTrue(
            np.allclose(input[0, :, 0:2].numpy(), x[0, :, 0:2], 1e-5, 1e-5)
        )


@unittest.skipIf(
    not flow.unittest.env.eager_execution_enabled(),
    ".numpy() doesn't work in lazy mode",
)
class TestSliceUpdate(flow.unittest.TestCase):
    def test_slice_update(test_case):
        x = np.array([1, 1, 1, 1, 1]).astype(np.float32)
        input = flow.Tensor(x)
        update = flow.Tensor(np.array([2, 3, 4]).astype(np.float32))
        output = np.array([1.0, 2.0, 3.0, 4.0, 1.0])
        y = flow.tmp.slice_update(input, update, slice_tup_list=[[1, 4, 1]])
        test_case.assertTrue(np.array_equal(y.numpy(), output))

    def test_tensor_slice_update(test_case):
        x = np.random.randn(2, 3, 4, 5).astype(np.float32)
        input = flow.Tensor(x)
        input[0] = 3.1415
        x[0] = 3.1415
        # TODO：False,input seem not set value success!
        test_case.assertTrue(np.allclose(input[0].numpy(), x[0], 1e-5, 1e-5))


if __name__ == "__main__":
    unittest.main()
