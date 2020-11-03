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

namespace oneflow {

REGISTER_USER_OP("partial_fc_sample")
    .Input("weight")
    .Input("label")
    .Output("maped_label")
    .Output("sampled_label")
    .Output("sampled_weight")
    .Attr<int64_t>("num_sample")
    .Attr<bool>("indexed_slice_update")
    .SetTensorDescInferFn([](user_op::InferContext* ctx) -> Maybe<void> {
      const int64_t num_sample = ctx->Attr<int64_t>("num_sample");
      const int64_t parallel_num = ctx->parallel_ctx().parallel_num();
      const int64_t num_sample_per_rank = RoundUp(num_sample, parallel_num) / parallel_num;
      const user_op::TensorDesc* weight = ctx->TensorDesc4ArgNameAndIndex("weight", 0);
      const user_op::TensorDesc* label = ctx->TensorDesc4ArgNameAndIndex("label", 0);
      user_op::TensorDesc* sampled_weight = ctx->TensorDesc4ArgNameAndIndex("sampled_weight", 0);
      user_op::TensorDesc* sampled_label = ctx->TensorDesc4ArgNameAndIndex("sampled_label", 0);
      *ctx->TensorDesc4ArgNameAndIndex("maped_label", 0) = *label;
      *sampled_weight = *weight;
      sampled_weight->mut_shape()->Set(0, num_sample_per_rank);
      *sampled_label = *label;
      sampled_label->mut_shape()->Set(0, num_sample_per_rank);
      return Maybe<void>::Ok();
    })
    .SetBatchAxisInferFn([](user_op::BatchAxisContext* ctx) -> Maybe<void> {
      *ctx->BatchAxis4ArgNameAndIndex("maped_label", 0) =
          *ctx->BatchAxis4ArgNameAndIndex("label", 0);
      ctx->BatchAxis4ArgNameAndIndex("sampled_label", 0)->clear_value();
      ctx->BatchAxis4ArgNameAndIndex("sampled_weight", 0)->clear_value();
      return Maybe<void>::Ok();
    })
    .SetInputArgModifyFn([](user_op::GetInputArgModifier GetInputArgModifierFn,
                            const user_op::UserOpConfWrapper&) {
      user_op::InputArgModifier* label_modifier = GetInputArgModifierFn("label", 0);
      CHECK_NOTNULL(label_modifier);
      label_modifier->set_requires_grad(false);
    })
    .SetGetSbpFn([](user_op::SbpContext* ctx) -> Maybe<void> {
      ctx->NewBuilder()
          .Split(user_op::OpArg("weight", 0), 0)
          .Broadcast(user_op::OpArg("label", 0))
          .PartialSum(user_op::OpArg("maped_label", 0))
          .Split(user_op::OpArg("sampled_label", 0), 0)
          .Split(user_op::OpArg("sampled_weight", 0), 0)
          .Build();
      return Maybe<void>::Ok();
    });

REGISTER_USER_OP("partial_fc_sample_grad")
    .Input("sampled_weight_diff")
    .Input("sampled_label")
    .Input("weight")
    .Output("weight_diff")
    .SetTensorDescInferFn([](user_op::InferContext* ctx) -> Maybe<void> {
      *ctx->TensorDesc4ArgNameAndIndex("weight_diff", 0) =
          *ctx->TensorDesc4ArgNameAndIndex("weight", 0);
      return Maybe<void>::Ok();
    })
    .SetBatchAxisInferFn([](user_op::BatchAxisContext* ctx) -> Maybe<void> {
      ctx->BatchAxis4ArgNameAndIndex("weight_diff", 0)->clear_value();
      return Maybe<void>::Ok();
    })
    .SetInputArgModifyFn([](user_op::GetInputArgModifier GetInputArgModifierFn,
                            const user_op::UserOpConfWrapper&) {
      user_op::InputArgModifier* weight_modifier = GetInputArgModifierFn("weight", 0);
      CHECK_NOTNULL(weight_modifier);
      weight_modifier->set_use_header_only(true);
    })
    .SetGetSbpFn([](user_op::SbpContext* ctx) -> Maybe<void> {
      ctx->NewBuilder()
          .Split(user_op::OpArg("sampled_weight_diff", 0), 0)
          .Split(user_op::OpArg("sampled_label", 0), 0)
          .Split(user_op::OpArg("weight", 0), 0)
          .Split(user_op::OpArg("weight_diff", 0), 0)
          .Build();
      return Maybe<void>::Ok();
    });

REGISTER_USER_OP_GRAD("partial_fc_sample")
    .SetBackwardOpConfGenFn([](user_op::BackwardOpConfContext* ctx) {
      const auto grad_op_name = ctx->FwOp().op_name() + "_grad";
      const bool indexed_slice_update = ctx->FwOp().attr<bool>("indexed_slice_update");
      if (indexed_slice_update) {
        ctx->DefineOp(grad_op_name, [&ctx](user_op::BackwardOpBuilder& builder) {
          return builder.OpTypeName("unsorted_segment_sum_like")
              .InputBind("data", ctx->FwOp().output_grad("sampled_weight", 0))
              .InputBind("segment_ids", ctx->FwOp().output("sampled_label", 0))
              .InputBind("like", ctx->FwOp().input("weight", 0))
              .Output("out")
              .Attr("axis", static_cast<int64_t>(0))
              .Build();
        });
        ctx->FwOp().InputGradBind(user_op::OpArg("weight", 0),
                                  [&ctx, &grad_op_name]() -> const std::string& {
                                    return ctx->GetOp(grad_op_name).output("out", 0);
                                  });
      } else {
        ctx->DefineOp(grad_op_name, [&ctx](user_op::BackwardOpBuilder& builder) {
          return builder.OpTypeName("partial_fc_sample_grad")
              .InputBind("sampled_weight_diff", ctx->FwOp().output_grad("sampled_weight", 0))
              .InputBind("sampled_label", ctx->FwOp().output("sampled_label", 0))
              .InputBind("weight", ctx->FwOp().input("weight", 0))
              .Output("weight_diff")
              .Build();
        });
        ctx->FwOp().InputGradBind(user_op::OpArg("weight", 0),
                                  [&ctx, &grad_op_name]() -> const std::string& {
                                    return ctx->GetOp(grad_op_name).output("weight_diff", 0);
                                  });
      }
    });

}  // namespace oneflow
