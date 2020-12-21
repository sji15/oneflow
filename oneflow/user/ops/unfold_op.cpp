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
#include "oneflow/core/framework/framework.h"
#include "oneflow/user/utils/unfold_util.h"

namespace oneflow {

namespace user_op {

namespace {

typedef std::function<Maybe<void>(user_op::InferContext* ctx)> TensorDescInferFn;
typedef std::function<void(const user_op::UserOpWrapper& op, user_op::AddOpFn AddOp)>
    GenBackwardOpConfFn;

TensorDescInferFn MakeFwTensorDescInferFn(const int32_t dim) {
  return [dim](user_op::InferContext* ctx) -> Maybe<void> {
    const Shape* x_shape = ctx->Shape4ArgNameAndIndex("x", 0);
    const std::string& data_format = ctx->Attr<std::string>("data_format");
    const std::string& padding = ctx->Attr<std::string>("padding");
    const auto& padding_before = ctx->Attr<std::vector<int32_t>>("padding_before");
    const auto& padding_after = ctx->Attr<std::vector<int32_t>>("padding_after");
    const std::vector<int32_t> kernel_size = ctx->Attr<std::vector<int32_t>>("kernel_size");
    const std::vector<int32_t> strides = ctx->Attr<std::vector<int32_t>>("strides");
    const std::vector<int32_t> dilation_rate = ctx->Attr<std::vector<int32_t>>("dilation_rate");
    const bool ceil_mode = ctx->Attr<bool>("ceil_mode");

    CHECK_EQ_OR_RETURN(kernel_size.size(), dim);
    for (int32_t kernel_dim : kernel_size) { CHECK_GT_OR_RETURN(kernel_dim, 0); }
    CHECK_EQ_OR_RETURN(strides.size(), dim);
    for (int32_t stride_dim : strides) { CHECK_GT_OR_RETURN(stride_dim, 0); }
    CHECK_EQ_OR_RETURN(dilation_rate.size(), dim);
    for (int32_t dilation_dim : dilation_rate) { CHECK_GT_OR_RETURN(dilation_dim, 0); }

    const ParamsUnfold3D params_3d(dim, *x_shape, data_format, padding, padding_before,
                                   padding_after, kernel_size, strides, dilation_rate, ceil_mode);
    user_op::TensorDesc* y_desc = ctx->TensorDesc4ArgNameAndIndex("y", 0);
    *y_desc = *ctx->TensorDesc4ArgNameAndIndex("x", 0);
    *y_desc->mut_shape() = params_3d.GetYShape();
    return Maybe<void>::Ok();
  };
}

Maybe<void> BwTensorDescInferFn(user_op::InferContext* ctx) {
  *ctx->TensorDesc4ArgNameAndIndex("dx", 0) = *ctx->TensorDesc4ArgNameAndIndex("x", 0);
  return Maybe<void>::Ok();
}

Maybe<void> FwBatchAxisInferFn(user_op::BatchAxisContext* ctx) {
  *ctx->BatchAxis4ArgNameAndIndex("y", 0) = *ctx->BatchAxis4ArgNameAndIndex("x", 0);
  return Maybe<void>::Ok();
}

Maybe<void> BwBatchAxisInferFn(user_op::BatchAxisContext* ctx) {
  *ctx->BatchAxis4ArgNameAndIndex("dx", 0) = *ctx->BatchAxis4ArgNameAndIndex("x", 0);
  return Maybe<void>::Ok();
}

Maybe<void> FwGetSbpFn(user_op::SbpContext* ctx) {
  const std::string& data_format = ctx->Attr<std::string>("data_format");

  ctx->NewBuilder().Split(user_op::OpArg("x", 0), 0).Split(user_op::OpArg("y", 0), 0).Build();
  if (data_format == "channels_first") {
    ctx->NewBuilder().Split(user_op::OpArg("x", 0), 1).Split(user_op::OpArg("y", 0), 1).Build();
  }
  return Maybe<void>::Ok();
}

Maybe<void> BwGetSbpFn(user_op::SbpContext* ctx) {
  const std::string& data_format = ctx->Attr<std::string>("data_format");

  ctx->NewBuilder()
      .Split(user_op::OpArg("x", 0), 0)
      .Split(user_op::OpArg("y", 0), 0)
      .Split(user_op::OpArg("dy", 0), 0)
      .Split(user_op::OpArg("dx", 0), 0)
      .Build();
  if (data_format == "channels_first") {
    ctx->NewBuilder()
        .Split(user_op::OpArg("x", 0), 1)
        .Split(user_op::OpArg("y", 0), 1)
        .Split(user_op::OpArg("dy", 0), 1)
        .Split(user_op::OpArg("dx", 0), 1)
        .Build();
  }
  return Maybe<void>::Ok();
}

GenBackwardOpConfFn MakeGenBackwardOpConfFn(const int32_t dim) {
  return [dim](const user_op::UserOpWrapper& op, user_op::AddOpFn AddOp) {
    if (op.NeedGenGradTensor4OpInput("x", 0)) {
      user_op::UserOpConfWrapperBuilder builder(op.op_name() + "_grad");
      user_op::UserOpConfWrapper grad_op =
          builder.Op("unfold_" + std::to_string(dim) + "d_grad")
              .Input("x", op.input("x", 0))
              .Input("y", op.output("y", 0))
              .Input("dy", op.GetGradTensorWithOpOutput("y", 0))
              .Output("dx")
              .Attr("data_format", op.attr<std::string>("data_format"))
              .Attr("padding", op.attr<std::string>("padding"))
              .Attr("padding_before", op.attr<std::vector<int32_t>>("padding_before"))
              .Attr("padding_after", op.attr<std::vector<int32_t>>("padding_after"))
              .Attr("kernel_size", op.attr<std::vector<int32_t>>("kernel_size"))
              .Attr("strides", op.attr<std::vector<int32_t>>("strides"))
              .Attr("dilation_rate", op.attr<std::vector<int32_t>>("dilation_rate"))
              .Attr("ceil_mode", op.attr<bool>("ceil_mode"))
              .Build();
      op.BindGradTensorWithOpInput(grad_op.output("dx", 0), "x", 0);
      AddOp(grad_op);
    }
  };
}

}  // namespace

REGISTER_USER_OP("unfold_1d")
    .Input("x")
    .Output("y")
    .Attr<std::string>("padding")
    .Attr<std::vector<int32_t>>("padding_before")
    .Attr<std::vector<int32_t>>("padding_after")
    .Attr<std::string>("data_format")
    .Attr<std::vector<int32_t>>("kernel_size")
    .Attr<std::vector<int32_t>>("strides")
    .Attr<std::vector<int32_t>>("dilation_rate")
    .Attr<bool>("ceil_mode")
    .SetTensorDescInferFn(MakeFwTensorDescInferFn(1))
    .SetBatchAxisInferFn(FwBatchAxisInferFn)
    .SetGetSbpFn(FwGetSbpFn);

REGISTER_USER_OP("unfold_1d_grad")
    .Input("x")
    .Input("y")
    .Input("dy")
    .Output("dx")
    .Attr<std::string>("padding")
    .Attr<std::vector<int32_t>>("padding_before")
    .Attr<std::vector<int32_t>>("padding_after")
    .Attr<std::string>("data_format")
    .Attr<std::vector<int32_t>>("kernel_size")
    .Attr<std::vector<int32_t>>("strides")
    .Attr<std::vector<int32_t>>("dilation_rate")
    .Attr<bool>("ceil_mode")
    .SetTensorDescInferFn(BwTensorDescInferFn)
    .SetBatchAxisInferFn(BwBatchAxisInferFn)
    .SetGetSbpFn(BwGetSbpFn);

REGISTER_USER_OP_GRAD("unfold_1d").SetGenBackwardOpConfFn(MakeGenBackwardOpConfFn(1));

REGISTER_USER_OP("unfold_2d")
    .Input("x")
    .Output("y")
    .Attr<std::string>("padding")
    .Attr<std::vector<int32_t>>("padding_before")
    .Attr<std::vector<int32_t>>("padding_after")
    .Attr<std::string>("data_format")
    .Attr<std::vector<int32_t>>("kernel_size")
    .Attr<std::vector<int32_t>>("strides")
    .Attr<std::vector<int32_t>>("dilation_rate")
    .Attr<bool>("ceil_mode")
    .SetTensorDescInferFn(MakeFwTensorDescInferFn(2))
    .SetBatchAxisInferFn(FwBatchAxisInferFn)
    .SetGetSbpFn(FwGetSbpFn);

REGISTER_USER_OP("unfold_2d_grad")
    .Input("x")
    .Input("y")
    .Input("dy")
    .Output("dx")
    .Attr<std::string>("padding")
    .Attr<std::vector<int32_t>>("padding_before")
    .Attr<std::vector<int32_t>>("padding_after")
    .Attr<std::string>("data_format")
    .Attr<std::vector<int32_t>>("kernel_size")
    .Attr<std::vector<int32_t>>("strides")
    .Attr<std::vector<int32_t>>("dilation_rate")
    .Attr<bool>("ceil_mode")
    .SetTensorDescInferFn(BwTensorDescInferFn)
    .SetBatchAxisInferFn(BwBatchAxisInferFn)
    .SetGetSbpFn(BwGetSbpFn);

REGISTER_USER_OP_GRAD("unfold_2d").SetGenBackwardOpConfFn(MakeGenBackwardOpConfFn(2));

REGISTER_USER_OP("unfold_3d")
    .Input("x")
    .Output("y")
    .Attr<std::string>("padding")
    .Attr<std::vector<int32_t>>("padding_before")
    .Attr<std::vector<int32_t>>("padding_after")
    .Attr<std::string>("data_format")
    .Attr<std::vector<int32_t>>("kernel_size")
    .Attr<std::vector<int32_t>>("strides")
    .Attr<std::vector<int32_t>>("dilation_rate")
    .Attr<bool>("ceil_mode")
    .SetTensorDescInferFn(MakeFwTensorDescInferFn(3))
    .SetBatchAxisInferFn(FwBatchAxisInferFn)
    .SetGetSbpFn(FwGetSbpFn);

REGISTER_USER_OP("unfold_3d_grad")
    .Input("x")
    .Input("y")
    .Input("dy")
    .Output("dx")
    .Attr<std::string>("padding")
    .Attr<std::vector<int32_t>>("padding_before")
    .Attr<std::vector<int32_t>>("padding_after")
    .Attr<std::string>("data_format")
    .Attr<std::vector<int32_t>>("kernel_size")
    .Attr<std::vector<int32_t>>("strides")
    .Attr<std::vector<int32_t>>("dilation_rate")
    .Attr<bool>("ceil_mode")
    .SetTensorDescInferFn(BwTensorDescInferFn)
    .SetBatchAxisInferFn(BwBatchAxisInferFn)
    .SetGetSbpFn(BwGetSbpFn);

REGISTER_USER_OP_GRAD("unfold_3d").SetGenBackwardOpConfFn(MakeGenBackwardOpConfFn(3));

}  // namespace user_op

}  // namespace oneflow