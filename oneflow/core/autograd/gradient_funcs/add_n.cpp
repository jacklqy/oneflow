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
#include "oneflow/core/framework/op_expr_grad_function.h"

namespace oneflow {
namespace one {

struct AddNCaptureState : public AutoGradCaptureState {
  int32_t input_num;
  std::vector<bool> requires_grad;
};

class AddN : public OpExprGradFunction<AddNCaptureState> {
 public:
  Maybe<void> Init(const OpExpr& op) override { return Maybe<void>::Ok(); }

  Maybe<void> Capture(AddNCaptureState* ctx, const TensorTuple& inputs, const TensorTuple& outputs,
                      const AttrMap& attrs) const override {
    ctx->input_num = inputs.size();
    ctx->requires_grad.resize(inputs.size());
    for (int i = 0; i < inputs.size(); ++i) {
      ctx->requires_grad[i] = inputs.at(i)->requires_grad();
    }
    return Maybe<void>::Ok();
  }

  Maybe<void> Apply(const AddNCaptureState* ctx, const TensorTuple& out_grads,
                    TensorTuple* in_grads) const override {
    CHECK_EQ_OR_RETURN(out_grads.size(), 1);
    in_grads->resize(ctx->input_num);
    for (int i = 0; i < ctx->input_num; ++i) {
      if (ctx->requires_grad.at(i)) { in_grads->at(i) = out_grads.at(0); }
    }
    return Maybe<void>::Ok();
  }
};

REGISTER_OP_EXPR_GRAD_FUNCTION("add_n", AddN);

}  // namespace one
}  // namespace oneflow
