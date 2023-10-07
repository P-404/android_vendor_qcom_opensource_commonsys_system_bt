/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

    * Redistribution and use in source and binary forms, with or without
      modification, are permitted (subject to the limitations in the
      disclaimer below) provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

#pragma once

#include "client_interface.h"
#include "../audio_a2dp_hw/include/audio_a2dp_hw.h"
#include "btif_ahim.h"
#include "btif_av.h"

namespace bluetooth {
namespace audio {
namespace aidl {
namespace a2dp {

namespace {

BluetoothAudioCtrlAck a2dp_ack_to_bt_audio_ctrl_ack(tA2DP_CTRL_ACK ack);

// Provide call-in APIs for the Bluetooth Audio HAL
class A2dpTransport
    : public ::bluetooth::audio::aidl::IBluetoothSinkTransportInstance {
 public:
  A2dpTransport(SessionType sessionType);

  BluetoothAudioCtrlAck StartRequest(bool is_low_latency) override;

  BluetoothAudioCtrlAck SuspendRequest() override;

  void StopRequest() override;

  bool GetPresentationPosition(uint64_t* remote_delay_report_ns,
                               uint64_t* total_bytes_read,
                               timespec* data_position) override;

  void SourceMetadataChanged(const source_metadata_t& source_metadata);

  void SinkMetadataChanged(const sink_metadata_t&) override;

  tA2DP_CTRL_CMD GetPendingCmd() const;

  void ResetPendingCmd();

  void ResetPresentationPosition();

  void LogBytesRead(size_t bytes_read) override;

  // delay reports from AVDTP is based on 1/10 ms (100us)
  void SetRemoteDelay(uint16_t delay_report);

 private:
  tA2DP_CTRL_ACK ProcessRequest(tA2DP_CTRL_CMD cmd) {
    btif_dispatch_sm_event(BTIF_AV_PROCESS_HIDL_REQ_EVT, (char*)&cmd,
                           sizeof(cmd));
    return A2DP_CTRL_ACK_PENDING;
  }
  static tA2DP_CTRL_CMD a2dp_pending_cmd_;
  static uint16_t remote_delay_report_;
  uint64_t total_bytes_read_;
  timespec data_position_;
};
}  // namespace

}  // namespace a2dp
}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth
