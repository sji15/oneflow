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
#include "oneflow/core/common/util.h"
#include "oneflow/core/object_msg/flat_msg_view.h"
#include "oneflow/core/job/parallel_desc.h"
#include "oneflow/core/vm/instruction.msg.h"
#include "oneflow/core/vm/instruction_type.h"
#include "oneflow/core/vm/string_object.h"
#include "oneflow/core/eager/blob_instruction_type.h"
#include "oneflow/core/eager/blob_object.h"
#include "oneflow/core/vm/device_helper_stream_type.h"
#include "oneflow/core/device/cuda_util.h"
#include "oneflow/core/register/register_manager.h"
#include "oneflow/core/eager/lazy_ref_blob_object.h"
#include "oneflow/core/operator/operator.h"
#include "oneflow/core/vm/access_blob_arg_cb_phy_instr_operand.h"
#include "oneflow/core/register/ofblob.h"
#include "oneflow/core/eager/eager_blob_object.h"
#include "oneflow/core/vm/cuda_copy_d2h_stream_type.h"
#include "oneflow/core/vm/cuda_copy_h2d_stream_type.h"
#include "oneflow/core/vm/copy_blob_to_other_device_phy_instr_operand.h"
#include "oneflow/core/framework/tensor.h"

namespace oneflow {
namespace eager {

namespace {

// clang-format off
FLAT_MSG_VIEW_BEGIN(PinBlobInstruction);
  FLAT_MSG_VIEW_DEFINE_PATTERN(vm::MutOperand, blob);
FLAT_MSG_VIEW_END(PinBlobInstruction);
// clang-format on

}  // namespace

#ifdef WITH_CUDA
class CudaHostRegisterBlobInstructionType final : public vm::InstructionType {
 public:
  CudaHostRegisterBlobInstructionType() = default;
  ~CudaHostRegisterBlobInstructionType() override = default;

  using stream_type = vm::DeviceHelperStreamType;

  void Infer(vm::Instruction* instruction) const override {
    // do nothing
  }
  void Compute(vm::Instruction* instruction) const override {
    FlatMsgView<PinBlobInstruction> args(instruction->instr_msg().operand());
    auto* blob_obj = CHECK_JUST(instruction->mut_operand_type(args->blob())->Mut<BlobObject>());
    auto* blob = blob_obj->mut_blob();
    CHECK(blob->mem_case().has_host_mem());
    if (blob->mem_case().host_mem().has_cuda_pinned_mem()) { return; }
    void* dptr = blob->mut_dptr();
    CHECK_NOTNULL(dptr);
    size_t size = blob->AlignedByteSizeOfBlobBody();
    cudaError_t cuda_error = cudaHostRegister(dptr, size, cudaHostRegisterDefault);
    if (cuda_error == cudaErrorHostMemoryAlreadyRegistered) {
      cudaGetLastError();
      return;
    }
    OF_CUDA_CHECK(cuda_error);
  }
};
COMMAND(vm::RegisterInstructionType<CudaHostRegisterBlobInstructionType>("CudaHostRegisterBlob"));

class CudaHostUnregisterBlobInstructionType final : public vm::InstructionType {
 public:
  CudaHostUnregisterBlobInstructionType() = default;
  ~CudaHostUnregisterBlobInstructionType() override = default;

  using stream_type = vm::DeviceHelperStreamType;

  void Infer(vm::Instruction* instruction) const override {
    // do nothing
  }
  void Compute(vm::Instruction* instruction) const override {
    FlatMsgView<PinBlobInstruction> args(instruction->instr_msg().operand());
    auto* blob_obj = CHECK_JUST(instruction->mut_operand_type(args->blob())->Mut<BlobObject>());
    auto* blob = blob_obj->mut_blob();
    CHECK(blob->mem_case().has_host_mem());
    if (blob->mem_case().host_mem().has_cuda_pinned_mem()) { return; }
    void* dptr = blob->mut_dptr();
    CHECK_NOTNULL(dptr);
    cudaError_t cuda_error = cudaHostUnregister(dptr);
    if (cuda_error == cudaErrorHostMemoryNotRegistered) {
      cudaGetLastError();
      return;
    }
    OF_CUDA_CHECK(cuda_error);
  }
};
COMMAND(
    vm::RegisterInstructionType<CudaHostUnregisterBlobInstructionType>("CudaHostUnregisterBlob"));

void CopyBlobToOtherDeviceInstructionType::Infer(vm::Instruction* instruction) const {
  const vm::InstructionMsg& instr_msg = instruction->instr_msg();
  const auto& phy_instr_operand = instr_msg.phy_instr_operand();
  CHECK(static_cast<bool>(phy_instr_operand));
  const auto* ptr =
      dynamic_cast<const vm::CopyBlobToOtherDevicePhyInstrOperand*>(phy_instr_operand.get());
  CHECK_NOTNULL(ptr);
  const std::shared_ptr<one::MirroredTensor>& src_tensor = ptr->src_tensor();
  const std::shared_ptr<one::MirroredTensor>& dst_tensor = ptr->dst_tensor();
  dst_tensor->set_shape(src_tensor->shape());
  dst_tensor->set_requires_grad(src_tensor->requires_grad());
  dst_tensor->set_retain_grad(src_tensor->retain_grad());
  dst_tensor->set_is_leaf(false);
  dst_tensor->set_dtype(src_tensor->dtype());
}

namespace {

void RegisterMemory(Blob* blob) {
  void* register_dptr = blob->mut_dptr();
  CHECK_NOTNULL(register_dptr);
  if (blob->mem_case().host_mem().has_cuda_pinned_mem()) {
    size_t size = blob->AlignedByteSizeOfBlobBody();
    cudaError_t cuda_error = cudaHostRegister(register_dptr, size, cudaHostRegisterDefault);
    if (cuda_error == cudaErrorHostMemoryAlreadyRegistered) {
      cudaGetLastError();
      return;
    }
    OF_CUDA_CHECK(cuda_error);
  }
}

void UnRegisterMemory(Blob* blob) {
  void* register_dptr = blob->mut_dptr();
  CHECK_NOTNULL(register_dptr);
  if (blob->mem_case().host_mem().has_cuda_pinned_mem()) {
    cudaError_t cuda_error = cudaHostUnregister(register_dptr);
    if (cuda_error == cudaErrorHostMemoryNotRegistered) {
      cudaGetLastError();
      return;
    }
    OF_CUDA_CHECK(cuda_error);
  }
}
}  // namespace

Maybe<void> CopyBlobToOtherDeviceInstructionType::Run(vm::Instruction* instruction) const {
  const vm::InstructionMsg& instr_msg = instruction->instr_msg();
  const auto& phy_instr_operand = instr_msg.phy_instr_operand();
  CHECK(static_cast<bool>(phy_instr_operand));
  const auto* ptr =
      dynamic_cast<const vm::CopyBlobToOtherDevicePhyInstrOperand*>(phy_instr_operand.get());
  CHECK_NOTNULL(ptr);
  DeviceCtx* device_ctx = instruction->stream().device_ctx().get();
  const std::shared_ptr<one::MirroredTensor>& src_tensor = ptr->src_tensor();
  const std::shared_ptr<one::MirroredTensor>& dst_tensor = ptr->dst_tensor();
  Blob* src_blob = JUST(src_tensor->eager_blob_object())->mut_blob();
  Blob* dst_blob = JUST(dst_tensor->eager_blob_object())->mut_blob();
  CHECK_EQ(src_blob->ByteSizeOfBlobBody(), dst_blob->ByteSizeOfBlobBody());
  Blob* register_blob = nullptr;
  if (src_tensor->is_cuda() && !dst_tensor->is_cuda()) {
    register_blob = dst_blob;
  } else if (!src_tensor->is_cuda() && dst_tensor->is_cuda()) {
    register_blob = src_blob;
  }
  RegisterMemory(register_blob);
  SyncAutoMemcpy(device_ctx, dst_blob->mut_dptr(), src_blob->dptr(), src_blob->ByteSizeOfBlobBody(),
                 src_blob->mem_case(), dst_blob->mem_case());
  UnRegisterMemory(register_blob);
  return Maybe<void>::Ok();
}

class CpuCopyBlobToGpuInstructionType final : public CopyBlobToOtherDeviceInstructionType {
 public:
  CpuCopyBlobToGpuInstructionType() = default;
  ~CpuCopyBlobToGpuInstructionType() = default;

  using stream_type = vm::CudaCopyH2DStreamType;
};
COMMAND(vm::RegisterInstructionType<CpuCopyBlobToGpuInstructionType>(
    "cpu.to.gpu.CopyBlobToOtherDevice"));

class GpuCopyBlobToCpuInstructionType final : public CopyBlobToOtherDeviceInstructionType {
 public:
  GpuCopyBlobToCpuInstructionType() = default;
  ~GpuCopyBlobToCpuInstructionType() = default;

  using stream_type = vm::CudaCopyD2HStreamType;
};
COMMAND(vm::RegisterInstructionType<GpuCopyBlobToCpuInstructionType>(
    "gpu.to.cpu.CopyBlobToOtherDevice"));

#endif

Maybe<void> LazyReferenceInstructionType::Run(vm::Instruction* instruction) const {
  FlatMsgView<LazyReferenceInstruction> args(instruction->instr_msg().operand());
  vm::RwMutexedObject* eager_blob_rw = instruction->mut_operand_type(args->eager_blob());
  const auto* lbn_operand = instruction->operand_type(args->lbn_sym_id());
  const auto lbn = JUST(lbn_operand->template Get<vm::StringObject>()).str();
  ParallelContext parallel_ctx;
  JUST(instruction->parallel_desc()->GetParallelContext(
      &parallel_ctx, instruction->stream().machine_id(), instruction->stream().device_id()));
  Blob* blob = Global<RegstMgr>::Get()->Blob4LbiAndParallelId(GenLogicalBlobId(lbn),
                                                              parallel_ctx.parallel_id());
  eager_blob_rw->Init<eager::LazyRefBlobObject>(blob);
  return Maybe<void>::Ok();
}

void AccessBlobByCallbackInstructionType::Compute(vm::Instruction* instruction) const {
  const vm::InstructionMsg& instr_msg = instruction->instr_msg();
  const auto& phy_instr_operand = instr_msg.phy_instr_operand();
  CHECK(static_cast<bool>(phy_instr_operand));
  const auto* ptr =
      dynamic_cast<const vm::AccessBlobArgCbPhyInstrOperand*>(phy_instr_operand.get());
  CHECK_NOTNULL(ptr);
  DeviceCtx* device_ctx = instruction->stream().device_ctx().get();
  OfBlob ofblob(device_ctx, ptr->eager_blob_object()->mut_blob());
  ptr->callback()(reinterpret_cast<uint64_t>(&ofblob));
}

}  // namespace eager
}  // namespace oneflow
