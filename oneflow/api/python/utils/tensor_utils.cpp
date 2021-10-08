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
#include "oneflow/api/python/utils/tensor_utils.h"

#include "oneflow/api/python/ofblob/ofblob.e.h"
#include "oneflow/core/autograd/autograd_engine.h"
#include "oneflow/core/common/container_util.h"
#include "oneflow/core/common/switch_func.h"
#include "oneflow/core/common/tensor_buffer.h"
#include "oneflow/core/framework/nd_sbp.h"
#include "oneflow/core/functional/functional.h"
#include "oneflow/extension/python/numpy.h"
#include "oneflow/core/framework/transport_util.h"
#include "oneflow/core/job/rank_group_scope.h"
#include "oneflow/core/common/decorator.h"

namespace py = pybind11;

namespace oneflow {
namespace one {

Maybe<void> EagerMirroredTensorZeros(const std::shared_ptr<Tensor>& t) {
  std::shared_ptr<MirroredTensor> local_tensor;
  if (t->is_local()) {
    local_tensor = JUST(t->AsMirroredTensor());
  } else {
    local_tensor = JUST(t->cur_rank_phy_tensor());
  }
  CHECK_OR_RETURN(local_tensor->is_eager()) << "eager tensors supported only";
  JUST(PhysicalRun([&](InstructionsBuilder* builder) -> Maybe<void> {
    JUST(builder->AccessBlobByCallback(
        local_tensor,
        [](uint64_t of_blob_ptr) {
          auto* of_blob = reinterpret_cast<OfBlob*>(of_blob_ptr);
          of_blob->AsyncAutoMemset(0);
        },
        "mut"));
    return Maybe<void>::Ok();
  }));
  return Maybe<void>::Ok();
}

template<typename T>
Maybe<void> CopyMirroredTensorFromUntypedArray(const std::shared_ptr<Tensor>& tensor,
                                               py::object array) {
  return CopyBetweenMirroredTensorAndNumpy(tensor, array.cast<py::array_t<T>>(),
                                           OfBlob_CopyFromBuffer, "mut");
}

Maybe<std::string> GetCopyMirroredTensorToNumpyFuncName(DataType dtype) {
  using namespace oneflow;
  static const HashMap<int64_t, std::shared_ptr<std::string>> data_type2func_name{
#define DATA_TYPE_FUNC_NAME_PAIR(type_cpp, type_proto) \
  {type_proto, std::make_shared<std::string>("_copy_to_numpy_" #type_cpp)},
      OF_PP_FOR_EACH_TUPLE(DATA_TYPE_FUNC_NAME_PAIR, POD_DATA_TYPE_SEQ)
#undef DATA_TYPE_FUNC_NAME_PAIR
  };
  return JUST(MapAt(data_type2func_name, static_cast<int64_t>(dtype)));
}

Maybe<std::string> GetCopyMirroredTensorFromNumpyFuncName(DataType dtype) {
  using namespace oneflow;
  static const HashMap<int64_t, std::shared_ptr<std::string>> data_type2func_name{
#define DATA_TYPE_FUNC_NAME_PAIR(type_cpp, type_proto) \
  {type_proto, std::make_shared<std::string>("_copy_from_numpy_" #type_cpp)},
      OF_PP_FOR_EACH_TUPLE(DATA_TYPE_FUNC_NAME_PAIR, POD_DATA_TYPE_SEQ)
#undef DATA_TYPE_FUNC_NAME_PAIR
  };
  return JUST(MapAt(data_type2func_name, static_cast<int64_t>(dtype)));
}

Maybe<std::tuple<std::vector<Shape>, std::vector<Symbol<DType>>>>
MaybeGetTensorBufferShapesAndDTypes(const std::shared_ptr<Tensor>& t) {
  const auto& tensor = JUST(t->AsMirroredTensor());
  CHECK_OR_RETURN(tensor->is_eager()) << "eager tensors supported only";
  std::vector<Shape> shapes;
  std::vector<Symbol<DType>> dtypes;

  const auto& Callback = std::make_shared<std::function<void(uint64_t)>>([](uint64_t) {});
  JUST(SpinCounter::SpinWait(1, [&](const std::shared_ptr<SpinCounter>& sc) -> Maybe<void> {
    return PhysicalRun([&](InstructionsBuilder* builder) -> Maybe<void> {
      return builder->SyncAccessBlobByCallback(tensor, sc, Callback, "const");
    });
  }));

  const Blob& blob = JUST(tensor->eager_blob_object())->blob();
  const Shape& blob_shape = blob.static_shape();
  const auto* tensor_buffer_ptr = blob.dptr<TensorBuffer>();
  for (int64_t i = 0; i < blob_shape.elem_cnt(); ++i) {
    const TensorBuffer* tensor_buffer = tensor_buffer_ptr + i;
    shapes.push_back(tensor_buffer->shape());
    dtypes.push_back(DType::Get(tensor_buffer->data_type()).GetOrThrow());
  }
  return std::make_tuple(shapes, dtypes);
}

Maybe<void> RegisterTensorHook(const std::shared_ptr<Tensor>& self,
                               const AutogradMeta::Hook& hook) {
  if (!self->grad_fn_node()) { JUST(AddAccumulateFunctionNode(self)); }
  self->mut_autograd_meta()->add_hook(hook);
  return Maybe<void>::Ok();
}

Maybe<py::tuple> TensorGetPyTupleOfSbp(const Tensor& tensor) {
  const auto& nd_sbp = JUST(tensor.nd_sbp());
  const auto& tuple = std::make_shared<py::tuple>(nd_sbp->sbp_parallel_size());
  for (int i = 0; i < nd_sbp->sbp_parallel_size(); ++i) {
    (*tuple)[i] = SymbolOf(nd_sbp->sbp_parallel(i));
  }
  return tuple;
}

#define MAKE_SWITCH_ENTRY(func_name, dtype) func_name<dtype>
DEFINE_STATIC_SWITCH_FUNC(Maybe<void>, CopyMirroredTensorFromUntypedArray, MAKE_SWITCH_ENTRY,
                          MAKE_DATA_TYPE_CTRV_SEQ(POD_DATA_TYPE_SEQ));

Maybe<Tensor> MakeLocalTensorFromData(PyObject* data, const Optional<Symbol<DType>>& dtype,
                                      const Optional<Symbol<Device>>& device, bool requires_grad) {
  auto* np_arr_pyobject = PyArray_FromAny(data, nullptr, 0, 0, NPY_ARRAY_DEFAULT, nullptr);
  if (!np_arr_pyobject) {
    return Error::RuntimeError() << "Can not convert input data to a numpy array.";
  }
  // transfer the ownership to np_arr_raii so that the ref count
  // can be decreased automatically when function exits either normally or abnormally
  auto np_arr_raii = py::reinterpret_steal<py::array>(np_arr_pyobject);
  auto* np_arr = reinterpret_cast<PyArrayObject*>(np_arr_pyobject);
  const npy_intp* dims_ptr = PyArray_SHAPE(np_arr);
  const Shape shape(DimVector(dims_ptr, dims_ptr + PyArray_NDIM(np_arr)));
  DataType data_type = JUST(numpy::GetOFDataTypeFromNpArray(np_arr));

  Symbol<Device> device_;
  if (device) {
    device_ = JUST(device);
  } else {
    device_ = JUST(Device::New("cpu"));
  }
  std::shared_ptr<Tensor> tensor =
      JUST(functional::Empty(shape, JUST(DType::Get(data_type)), device_));
  JUST(SwitchCopyMirroredTensorFromUntypedArray(SwitchCase(data_type), tensor, np_arr_raii));

  // Cast to float if data is double sequence, rather than numpy array.
  Symbol<DType> dtype_;
  if (dtype) {
    dtype_ = JUST(dtype);
  } else if (!dtype && data_type == DataType::kDouble && !PyArray_Check(data)) {
    dtype_ = DType::Float();
  }
  if (dtype_) { tensor = JUST(functional::Cast(tensor, dtype_)); }
  JUST(tensor->set_requires_grad(requires_grad));
  return tensor;
}

namespace {

Maybe<Symbol<cfg::NdSbp>> GetAllBroadcastNdSbp(size_t ndim) {
  cfg::NdSbp broadcast_nd_sbp;
  for (size_t i = 0; i < ndim; ++i) {
    broadcast_nd_sbp.mutable_sbp_parallel()->Add()->mutable_broadcast_parallel();
  }
  return SymbolOf(broadcast_nd_sbp);
}

auto* CachedGetAllBroadcastNdSbp = DECORATE(&GetAllBroadcastNdSbp, ThreadLocal);

template<typename T>
bool CheckVecEqual(size_t size, const T* in0, const T* in1) {
  for (size_t i = 0; i < size; ++i) {
    if (*(in0 + i) != *(in1 + i)) { return false; }
  }
  return true;
}

}  // namespace

template<typename T>
Maybe<void> DataConsistencyCheck(py::array_t<T> array, size_t elem_cnt,
                                 Symbol<ParallelDesc> placement) {
  const auto& rank_group = JUST(RankGroup::New(placement));
  size_t data_size = elem_cnt * sizeof(T);

  TransportToken transport_token = JUST(TransportToken::NewTransportToken(kTransportTokenTypeData));
  py::array contiguous_array = py::reinterpret_steal<py::array>(reinterpret_cast<PyObject*>(
      PyArray_GETCONTIGUOUS(reinterpret_cast<PyArrayObject*>(array.ptr()))));
  py::buffer_info buf = contiguous_array.request();
  T* buf_ptr = (T*)buf.ptr;
  size_t array_size = buf.size;
  CHECK_EQ_OR_RETURN(array_size, elem_cnt);

  std::vector<T> recv_buffer(elem_cnt);
  T* recv_ptr = recv_buffer.data();

  NaiveAsyncTransportCtx ctx(
      transport_token,
      [&](void** buffer, std::size_t* size, std::function<void()>* Cb) -> Maybe<void> {
        *buffer = reinterpret_cast<void*>(buf_ptr);
        *size = data_size;
        *Cb = [] {};
        return Maybe<void>::Ok();
      },
      [&](void** buffer, std::size_t* size, std::function<void()>* Cb) -> Maybe<void> {
        *buffer = recv_ptr;
        *size = data_size;
        *Cb = [] {};
        return Maybe<void>::Ok();
      });
  JUST(TransportUtil::SendToNextRankInRing(rank_group, transport_token, &ctx));
  JUST(TransportUtil::ReceiveFromPrevRankInRing(rank_group, transport_token, &ctx));
  JUST(TransportUtil::WaitUntilDoneOrTimeout(ctx, TransportUtil::TimeoutSeconds()));
  CHECK_OR_RETURN(CheckVecEqual(elem_cnt, buf_ptr, recv_ptr))
      << "Each rank must have same input sequence or numpy array";
  return Maybe<void>::Ok();
}

#define MAKE_SWITCH_ENTRY(func_name, dtype) func_name<dtype>
DEFINE_STATIC_SWITCH_FUNC(Maybe<void>, DataConsistencyCheck, MAKE_SWITCH_ENTRY,
                          MAKE_DATA_TYPE_CTRV_SEQ(POD_DATA_TYPE_SEQ));

Maybe<Tensor> MakeConsistentTensorFromData(PyObject* data, const Optional<Symbol<DType>>& dtype,
                                           Symbol<ParallelDesc> placement,
                                           const std::vector<Symbol<cfg::SbpParallel>>& sbp_tuple,
                                           bool requires_grad) {
  auto* np_arr_pyobject = PyArray_FromAny(data, nullptr, 0, 0, NPY_ARRAY_DEFAULT, nullptr);
  if (!np_arr_pyobject) {
    return Error::RuntimeError() << "Can not convert input data to a numpy array.";
  }
  // transfer the ownership to np_arr_raii so that the ref count
  // can be decreased automatically when function exits either normally or abnormally
  auto np_arr_raii = py::reinterpret_steal<py::array>(np_arr_pyobject);
  auto* np_arr = reinterpret_cast<PyArrayObject*>(np_arr_pyobject);
  const npy_intp* dims_ptr = PyArray_SHAPE(np_arr);
  const Shape shape(DimVector(dims_ptr, dims_ptr + PyArray_NDIM(np_arr)));
  DataType data_type = JUST(numpy::GetOFDataTypeFromNpArray(np_arr));

  JUST(SwitchDataConsistencyCheck(SwitchCase(data_type), np_arr_raii, shape.elem_cnt(), placement));

  const std::string& device_tag = placement->device_tag();
  Symbol<Device> device;
  if (device_tag == "cpu") {
    device = JUST(Device::New("cpu"));
  } else {
    device = JUST(Device::New("cuda"));
  }
  std::shared_ptr<Tensor> local_tensor =
      JUST(functional::Empty(shape, JUST(DType::Get(data_type)), device));
  JUST(SwitchCopyMirroredTensorFromUntypedArray(SwitchCase(data_type), local_tensor, np_arr_raii));

  // Cast to float if data is double sequence, rather than numpy array.
  Symbol<DType> dtype_;
  if (dtype) {
    dtype_ = JUST(dtype);
  } else if (!dtype && data_type == DataType::kDouble && !PyArray_Check(data)) {
    dtype_ = DType::Float();
  }
  if (dtype_) { local_tensor = JUST(functional::Cast(local_tensor, dtype_)); }
  JUST(local_tensor->set_requires_grad(requires_grad));

  size_t sbp_dims = sbp_tuple.size();
  Symbol<cfg::NdSbp> broadcast_nd_sbp = JUST(CachedGetAllBroadcastNdSbp(sbp_dims));

  std::shared_ptr<Tensor> broadcast_tensor = JUST(functional::LocalToConsistent(
      local_tensor, placement, *JUST(GetSbpList(broadcast_nd_sbp)), shape, dtype_));

  std::vector<Symbol<cfg::SbpParallel>> grad_sbp_tuple;
  return JUST(functional::ToConsistent(broadcast_tensor, placement, sbp_tuple, grad_sbp_tuple));
}

Maybe<Tensor> MakeTensorFromOtherTensor(const std::shared_ptr<Tensor>& other) {
  if (other->is_local()) {
    const Symbol<Device>& device = JUST(other->device());
    return functional::Copy(other, device->type(), device->device_id());
  } else {
    const Symbol<cfg::NdSbp>& nd_sbp = JUST(other->nd_sbp());
    std::vector<Symbol<cfg::SbpParallel>> sbp_tuple(nd_sbp->sbp_parallel().size());
    for (int i = 0; i < sbp_tuple.size(); ++i) { sbp_tuple[i] = nd_sbp->sbp_parallel().Get(i); }
    std::vector<Symbol<cfg::SbpParallel>> grad_sbp_tuple;
    return functional::ToConsistent(other, JUST(other->parallel_desc()), sbp_tuple, grad_sbp_tuple);
  }
}

Maybe<Tensor> MakeTensorFromOtherTensor(const std::shared_ptr<Tensor>& other,
                                        const Optional<Symbol<DType>>& dtype,
                                        const Optional<Symbol<Device>>& device,
                                        const bool& requires_grad) {
  std::shared_ptr<Tensor> tensor;
  Symbol<Device> device_;
  if (device) { device_ = JUST(device); }
  if (other->is_local()) {
    if (!device) { device_ = JUST(other->device()); }
    tensor = JUST(functional::Copy(other, device_->type(), device_->device_id()));
  } else {
    tensor = JUST(functional::ConsistentToLocal(other));
    if (!device) { device_ = JUST(Device::New("cpu")); }
    tensor = JUST(functional::Copy(tensor, device_->type(), device_->device_id()));
  }
  if (dtype) {
    const Symbol<DType>& dtype_ = JUST(dtype);
    if (tensor->dtype() != dtype_) { tensor = JUST(functional::Cast(tensor, dtype_)); }
  }
  JUST(tensor->set_requires_grad(requires_grad));
  return tensor;
}

Maybe<Tensor> MakeTensorFromOtherTensor(const std::shared_ptr<Tensor>& other,
                                        const Optional<Symbol<DType>>& dtype,
                                        const Symbol<ParallelDesc>& placement,
                                        const std::vector<Symbol<cfg::SbpParallel>>& sbp_tuple,
                                        const bool& requires_grad) {
  std::vector<Symbol<cfg::SbpParallel>> grad_sbp_tuple;
  std::shared_ptr<Tensor> tensor =
      JUST(functional::ToConsistent(other, placement, sbp_tuple, grad_sbp_tuple));
  if (dtype) {
    const Symbol<DType>& dtype_ = JUST(dtype);
    if (tensor->dtype() != dtype_) { tensor = JUST(functional::Cast(tensor, dtype_)); }
  }
  JUST(tensor->set_requires_grad(requires_grad));
  return tensor;
}

}  // namespace one
}  // namespace oneflow