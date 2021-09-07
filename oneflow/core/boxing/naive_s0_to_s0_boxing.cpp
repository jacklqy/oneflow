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
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/framework/nd_sbp.h"
#include "oneflow/core/boxing/eager_boxing_interpreter_util.h"
#include "oneflow/core/boxing/eager_boxing_interpreter.h"
#include "oneflow/core/common/decorator.h"
#include "oneflow/core/functional/functional.h"

namespace oneflow {

namespace {

bool IsSplitSbp(Symbol<cfg::SbpParallel> sbp_parallel, int64_t axis) {
  return sbp_parallel->has_split_parallel() && sbp_parallel->split_parallel().axis() == 0;
}

Maybe<void> RawCheckCclS0ToS0(Symbol<PlacedNdSbp> in, Symbol<PlacedNdSbp> out) {
  CHECK_EQ_OR_RETURN(in->nd_sbp()->sbp_parallel_size(), 1);
  CHECK_EQ_OR_RETURN(out->nd_sbp()->sbp_parallel_size(), 1);

  CHECK_OR_RETURN(IsSplitSbp(in->nd_sbp()->sbp_parallel(0), 0));
  CHECK_OR_RETURN(IsSplitSbp(out->nd_sbp()->sbp_parallel(0), 0));
  CHECK_EQ_OR_RETURN(in->placement()->device_tag(), out->placement()->device_tag());
  CHECK_EQ_OR_RETURN(in->placement()->device_type(), DeviceType::kCPU);
  return Maybe<void>::Ok();
}

static constexpr auto* CheckCclS0ToS0 = DECORATE(&RawCheckCclS0ToS0, ThreadLocal);

}  // namespace

Maybe<one::Tensor> CclS0ToS0(const std::shared_ptr<one::Tensor>& tensor, Symbol<PlacedNdSbp> in,
                             Symbol<PlacedNdSbp> out) {
  const auto& tensor_nd_sbp = JUST(tensor->nd_sbp());
  CHECK_OR_RETURN(tensor_nd_sbp == in->nd_sbp());
  const auto& tensor_placement = JUST(tensor->parallel_desc());
  CHECK_OR_RETURN(tensor_placement == in->placement());
  std::shared_ptr<one::Tensor> local_tensor = JUST(tensor->cur_rank_phy_tensor());

  local_tensor = JUST(one::functional::EagerS0ToS0(local_tensor, tensor_placement, out->placement(),
                                                   *tensor->shape()));
  const auto& sbp_list = JUST(GetSbpList(out->nd_sbp()));
  return JUST(one::functional::LocalToConsistent(local_tensor, out->placement(), *sbp_list,
                                                 *tensor->shape(), tensor->dtype()));
}

COMMAND(RegisterBoxingFunction("ccl-s0-to-s0", CheckCclS0ToS0, &CclS0ToS0));

}  // namespace oneflow