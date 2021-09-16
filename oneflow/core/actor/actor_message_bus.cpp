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
#include "oneflow/core/actor/actor_message_bus.h"
#include <cstdint>
#include <functional>
#include "oneflow/core/control/global_process_ctx.h"
#include "oneflow/core/device/collective_boxing_device_context.h"
#include "oneflow/core/job/id_manager.h"
#include "oneflow/core/thread/thread_manager.h"
#include "oneflow/core/comm_network/comm_network.h"

namespace oneflow {

void ActorMsgBus::SendMsg(const ActorMsg& msg) {
  int64_t dst_machine_id = Global<IDMgr>::Get()->MachineId4ActorId(msg.dst_actor_id());
  if (dst_machine_id == GlobalProcessCtx::Rank()) {
    SendMsgWithoutCommNet(msg);
  } else {
    CallBack cb = std::bind(&ActorMsgBus::HandleRecvData,this,std::placeholders::_1,std::placeholders::_2);
    if (msg.IsDataRegstMsgToConsumer()) {
      int64_t comm_net_sequence;
      {
        std::unique_lock<std::mutex> lock(
            regst_desc_id_dst_actor_id2comm_net_sequence_number_mutex_);
        int64_t& comm_net_sequence_ref =
            regst_desc_id_dst_actor_id2comm_net_sequence_number_[std::make_pair(
                msg.regst_desc_id(), msg.dst_actor_id())];
        comm_net_sequence = comm_net_sequence_ref;
        comm_net_sequence_ref += 1;
      }
      ActorMsg new_msg = msg;
      new_msg.set_comm_net_sequence_number(comm_net_sequence);
      //size_t size = 0;
      //uint64_t addr = Global<CommNet>::Get()->SerialActorMsgToData(new_msg, &size);
      //CHECK_EQ(size, sizeof(new_msg));
      size_t size = 0;
      char * serial_data =Global<CommNet>::Get()->SerialTokenToData(new_msg.regst()->comm_net_token(),&size);
      new_msg.AddUserData(size,serial_data);
      size = sizeof(new_msg);
      uint64_t addr = reinterpret_cast<uint64_t>(&new_msg);
      Global<CommNet>::Get()->SendMsg(dst_machine_id, addr, size);
    } else {
      /*size_t size = 0;
      uint64_t addr = Global<CommNet>::Get()->SerialActorMsgToData(msg, &size);
      CHECK_EQ(size, sizeof(msg));*/
      uint64_t addr = reinterpret_cast<uint64_t>(&msg);
      size_t size = sizeof(msg);
      Global<CommNet>::Get()->SendMsg(dst_machine_id, addr, size);
    }
  }
}

void ActorMsgBus::SendMsgWithoutCommNet(const ActorMsg& msg) {
  CHECK_EQ(Global<IDMgr>::Get()->MachineId4ActorId(msg.dst_actor_id()), GlobalProcessCtx::Rank());
  int64_t thrd_id = Global<IDMgr>::Get()->ThrdId4ActorId(msg.dst_actor_id());
  Global<ThreadMgr>::Get()->GetThrd(thrd_id)->EnqueueActorMsg(msg);
}

void ActorMsgBus::HandleRecvData(void *data, size_t size) {
   ActorMsg new_msg = Global<CommNet>::Get()->DeserialDataToActorMsg(data, size);
   SendMsgWithoutCommNet(new_msg);
}

}  // namespace oneflow
