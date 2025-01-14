/*
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
*/
#include <pybind11/pybind11.h>
#include "oneflow/api/python/of_api_registry.h"
#include "oneflow/api/python/ofblob/ofblob.e.h"
#include "oneflow/core/common/container_util.h"
#include "oneflow/core/framework/instructions_builder.h"
#include "oneflow/core/framework/tensor.h"
#include "oneflow/core/framework/device.h"
#include "oneflow/core/framework/py_distribute.h"
#include "oneflow/core/job/placement.cfg.h"
#include "oneflow/core/job/global_for.h"
#include "oneflow/core/framework/dtype.h"
#include "oneflow/core/autograd/autograd_engine.h"

namespace py = pybind11;

namespace oneflow {

namespace one {

namespace {

template<typename T>
struct TensorExportUtil final {};

template<>
struct TensorExportUtil<MirroredTensor> final {
  static std::shared_ptr<MirroredTensor> MakeTensor(const std::shared_ptr<const Shape>& shape,
                                                    const std::shared_ptr<const DType>& dtype,
                                                    const std::shared_ptr<const Device>& device,
                                                    bool is_lazy, bool requires_grad,
                                                    bool is_leaf) {
    return MirroredTensor::MakeTensor(shape, dtype, device, is_lazy, requires_grad, is_leaf);
  }
};

template<>
struct TensorExportUtil<ConsistentTensor> final {
  static std::shared_ptr<ConsistentTensor> MakeTensor(
      const std::shared_ptr<const Shape>& shape, const std::shared_ptr<const DType>& dtype,
      const std::shared_ptr<const compatible_py::Distribute>& distribute,
      const std::shared_ptr<const ParallelDesc>& parallel_desc, bool is_lazy, bool requires_grad,
      bool is_leaf) {
    return ConsistentTensor::MakeTensor(shape, dtype, distribute, parallel_desc, is_lazy,
                                        requires_grad, is_leaf);
  }
};

template<typename T>
void SpecializedDef(py::class_<T, Tensor, std::shared_ptr<T>>* api) {
  // do nothing
}

namespace {

template<typename T>
Maybe<void> CopyBetweenMirroredTensorAndNumpy(const std::shared_ptr<MirroredTensor>& tensor,
                                              py::array_t<T> array,
                                              void (*Copy)(uint64_t, py::array_t<T>),
                                              const std::string& modifier) {
  std::atomic<bool> synced(false);

  PhysicalRun([&](InstructionsBuilder* builder) {
    builder->AccessBlobByCallback(
        tensor,
        [&array, &synced, &Copy](uint64_t ofblob_ptr) {
          Copy(ofblob_ptr, array);
          synced = true;
        },
        modifier);
  });

  Global<ForeignLockHelper>::Get()->WithScopedRelease([&synced]() { /* spin wait */
                                                                    while (!synced) {}
  });

  return Maybe<void>::Ok();
}

template<typename T>
void ApiCopyMirroredTensorToNumpy(const std::shared_ptr<MirroredTensor>& tensor,
                                  py::array_t<T> array) {
  return CopyBetweenMirroredTensorAndNumpy(tensor, array, OfBlob_CopyToBuffer, "const")
      .GetOrThrow();
}

template<typename T>
void ApiCopyMirroredTensorFromNumpy(const std::shared_ptr<MirroredTensor>& tensor,
                                    py::array_t<T> array) {
  return CopyBetweenMirroredTensorAndNumpy(tensor, array, OfBlob_CopyFromBuffer, "mut")
      .GetOrThrow();
}

Maybe<std::string> GetCopyMirroredTensorToNumpyFuncName(const DType& dtype) {
  using namespace oneflow;
  static const HashMap<int64_t, std::shared_ptr<std::string>> data_type2func_name{
#define DATA_TYPE_FUNC_NAME_PAIR(type_cpp, type_proto) \
  {type_proto, std::make_shared<std::string>("_copy_to_numpy_" #type_cpp)},
      OF_PP_FOR_EACH_TUPLE(DATA_TYPE_FUNC_NAME_PAIR, POD_DATA_TYPE_SEQ)
#undef DATA_TYPE_FUNC_NAME_PAIR
  };
  return JUST(MapAt(data_type2func_name, static_cast<int64_t>(dtype.data_type())));
}

const std::string& ApiGetCopyMirroredTensorToNumpyFuncName(const Tensor& tensor) {
  return *GetCopyMirroredTensorToNumpyFuncName(*tensor.dtype()).GetPtrOrThrow();
}

Maybe<std::string> GetCopyMirroredTensorFromNumpyFuncName(const DType& dtype) {
  using namespace oneflow;
  static const HashMap<int64_t, std::shared_ptr<std::string>> data_type2func_name{
#define DATA_TYPE_FUNC_NAME_PAIR(type_cpp, type_proto) \
  {type_proto, std::make_shared<std::string>("_copy_from_numpy_" #type_cpp)},
      OF_PP_FOR_EACH_TUPLE(DATA_TYPE_FUNC_NAME_PAIR, POD_DATA_TYPE_SEQ)
#undef DATA_TYPE_FUNC_NAME_PAIR
  };
  return JUST(MapAt(data_type2func_name, static_cast<int64_t>(dtype.data_type())));
}

const std::string& ApiGetCopyMirroredTensorFromNumpyFuncName(const Tensor& tensor) {
  return *GetCopyMirroredTensorFromNumpyFuncName(*tensor.dtype()).GetPtrOrThrow();
}

}  // namespace

template<>
void SpecializedDef<MirroredTensor>(
    py::class_<MirroredTensor, Tensor, std::shared_ptr<MirroredTensor>>* api) {
#define DEFINE_TENSOR_METHOD(T, type_proto)                         \
  api->def("_copy_to_numpy_" #T, &ApiCopyMirroredTensorToNumpy<T>); \
  api->def("_copy_from_numpy_" #T, &ApiCopyMirroredTensorFromNumpy<T>);
  OF_PP_FOR_EACH_TUPLE(DEFINE_TENSOR_METHOD, POD_DATA_TYPE_SEQ);

#undef DEFINE_TENSOR_METHOD
  api->def("_get_copy_mirrored_tensor_to_numpy_func_name",
           &ApiGetCopyMirroredTensorToNumpyFuncName);
  api->def("_get_copy_mirrored_tensor_from_numpy_func_name",
           &ApiGetCopyMirroredTensorFromNumpyFuncName);
}

template<typename T>
void ExportTensor(py::module& m, const char* name) {
  py::class_<T, Tensor, std::shared_ptr<T>> tensor_api(m, name);
  tensor_api
      .def(py::init(&TensorExportUtil<T>::MakeTensor))
      // Properties of pytorch
      .def_property_readonly("shape", &T::shape)
      .def_property_readonly("device", &T::device)
      .def_property_readonly("is_cuda", &T::is_cuda)
      .def_property_readonly("dtype", &T::dtype)
      .def_property_readonly("data", &T::data)
      .def_property_readonly("grad", [](const T& t) { return t.api_acc_grad().GetPtrOrThrow(); })
      .def_property_readonly("grad_fn", &T::grad_fn_node)
      .def_property_readonly("requires_grad", &T::requires_grad)
      .def_property_readonly("is_leaf", &T::is_leaf)
      // Methods of pytorch
      .def("retain_grad",
           [](T& t) {
             if (!t.is_leaf()) { t.set_retain_grad(true); }
           })
      .def("detach", [](const T& t) { return t.api_detach().GetPtrOrThrow(); })
      // OneFlow tensor properties other than pytorch tensor
      .def_property_readonly("placement", &T::parallel_desc)
      .def_property_readonly("is_lazy", &T::is_lazy)
      .def_property_readonly("is_consistent", &T::is_consistent)
      .def_property_readonly("_blob_object", &T::blob_object)
      // OneFlow tensor methods other than pytorch tensor
      .def("_set_requires_grad", &T::set_requires_grad)
      .def("_set_blob_object", [](T& t, std::shared_ptr<compatible_py::BlobObject>& blob_object) {
        t.set_blob_object(blob_object).GetOrThrow();
      });
  SpecializedDef<T>(&tensor_api);
}

}  // namespace

ONEFLOW_API_PYBIND11_MODULE("", m) {
  py::class_<Tensor, std::shared_ptr<Tensor>>(m, "Tensor");
  ExportTensor<MirroredTensor>(m, "LocalTensor");
  ExportTensor<ConsistentTensor>(m, "ConsistentTensor");
}

}  // namespace one

}  // namespace oneflow
