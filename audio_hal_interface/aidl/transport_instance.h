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

#include <hardware/audio.h>

#if 0
typedef enum {
  LEA_CTRL_CMD_NONE,
  LEA_CTRL_CMD_CHECK_READY,
  LEA_CTRL_CMD_START,
  LEA_CTRL_CMD_STOP,
  LEA_CTRL_CMD_SUSPEND,
  LEA_CTRL_GET_INPUT_AUDIO_CONFIG,
  LEA_CTRL_GET_OUTPUT_AUDIO_CONFIG,
  LEA_CTRL_SET_OUTPUT_AUDIO_CONFIG,
  LEA_CTRL_GET_PRESENTATION_POSITION,
} tLEA_CTRL_CMD;

typedef enum {
  LEA_CTRL_ACK_SUCCESS,
  LEA_CTRL_ACK_FAILURE,
  LEA_CTRL_ACK_INCALL_FAILURE, /* Failure when in Call*/
  LEA_CTRL_ACK_UNSUPPORTED,
  LEA_CTRL_ACK_PENDING,
  LEA_CTRL_ACK_DISCONNECT_IN_PROGRESS,
} tLEA_CTRL_ACK;

#endif

//#include "audio_LEA_hw/include/audio_LEA_hw.h"
#include "audio_aidl_interfaces.h"
#include "audio_ctrl_ack.h"

namespace bluetooth {
namespace audio {
namespace aidl {

using ::aidl::android::hardware::bluetooth::audio::AudioConfiguration;
using ::aidl::android::hardware::bluetooth::audio::SessionType;

/***
 * An IBluetoothTransportInstance needs to be implemented by a Bluetooth
 * audio transport, such as LEA or Hearing Aid, to handle callbacks from Audio
 * HAL.
 ***/
class IBluetoothTransportInstance {
 public:
  IBluetoothTransportInstance(SessionType sessionType,
                              AudioConfiguration audioConfig)
      : session_type_(sessionType), audio_config_(std::move(audioConfig)){};
  virtual ~IBluetoothTransportInstance() = default;

  SessionType GetSessionType() const { return session_type_; }

  AudioConfiguration GetAudioConfiguration() const { return audio_config_; }

  void UpdateAudioConfiguration(const AudioConfiguration& audio_config) {
    LOG(ERROR) << __func__
              << "AIDL: UpdateAudioConfiguration IBluetoothTransportInstance";
    switch (audio_config.getTag()) {
      case AudioConfiguration::pcmConfig:
        audio_config_.set<AudioConfiguration::pcmConfig>(
            audio_config.get<AudioConfiguration::pcmConfig>());
        break;
      case AudioConfiguration::a2dpConfig:
        audio_config_.set<AudioConfiguration::a2dpConfig>(
            audio_config.get<AudioConfiguration::a2dpConfig>());
        break;
      case AudioConfiguration::leAudioConfig:
        LOG(ERROR) << __func__
                   << "AIDL: UpdateAudioConfiguration leAudioConfig";
        audio_config_.set<AudioConfiguration::leAudioConfig>(
            audio_config.get<AudioConfiguration::leAudioConfig>());
        break;
      case AudioConfiguration::leAudioBroadcastConfig:
        audio_config_.set<AudioConfiguration::leAudioBroadcastConfig>(
            audio_config.get<AudioConfiguration::leAudioBroadcastConfig>());
    }
  }

  virtual BluetoothAudioCtrlAck StartRequest(bool is_low_latency) = 0;

  virtual BluetoothAudioCtrlAck SuspendRequest() = 0;

  virtual void StopRequest() = 0;

  virtual bool GetPresentationPosition(uint64_t* remote_delay_report_ns,
                                       uint64_t* total_bytes_readed,
                                       timespec* data_position) = 0;

  virtual void SourceMetadataChanged(
      const source_metadata_t& source_metadata) = 0;
  virtual void SinkMetadataChanged(const sink_metadata_t& sink_metadata) = 0;

  //virtual tA2DP_CTRL_CMD GetPendingCmd() const;

  //virtual void ResetPendingCmd();
  /***
   * Invoked when the transport is requested to reset presentation position
   ***/
  virtual void ResetPresentationPosition() = 0;

 private:
  const SessionType session_type_;
  AudioConfiguration audio_config_;
};

/***
 * An IBluetoothSinkTransportInstance needs to be implemented by a Bluetooth
 * audio transport, such as LEA, Hearing Aid or LeAudio, to handle callbacks
 * from Audio HAL.
 ***/
class IBluetoothSinkTransportInstance : public IBluetoothTransportInstance {
 public:
  IBluetoothSinkTransportInstance(SessionType sessionType,
                                  AudioConfiguration audioConfig)
      : IBluetoothTransportInstance{sessionType, audioConfig} {}
  virtual ~IBluetoothSinkTransportInstance() = default;

  /***
   * Invoked when the transport is requested to log bytes read
   ***/
  virtual void LogBytesRead(size_t bytes_readed) = 0;
};

class IBluetoothSourceTransportInstance : public IBluetoothTransportInstance {
 public:
  IBluetoothSourceTransportInstance(SessionType sessionType,
                                    AudioConfiguration audioConfig)
      : IBluetoothTransportInstance{sessionType, audioConfig} {}
  virtual ~IBluetoothSourceTransportInstance() = default;

  /***
   * Invoked when the transport is requested to log bytes written
   ***/
  virtual void LogBytesWritten(size_t bytes_written) = 0;
};

}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth