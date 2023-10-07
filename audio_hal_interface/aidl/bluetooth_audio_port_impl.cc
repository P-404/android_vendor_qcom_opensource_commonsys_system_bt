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

#include "bluetooth_audio_port_impl.h"

#include "common/stop_watch_legacy.h"

namespace bluetooth {
namespace audio {
namespace aidl {

BluetoothAudioPortImpl::BluetoothAudioPortImpl(
    IBluetoothTransportInstance* transport_instance,
    const std::shared_ptr<IBluetoothAudioProvider>& provider)
    : transport_instance_(transport_instance), provider_(provider) {}

BluetoothAudioPortImpl::~BluetoothAudioPortImpl() {}

ndk::ScopedAStatus BluetoothAudioPortImpl::startStream(bool is_low_latency) {
  BluetoothAudioCtrlAck ack = transport_instance_->StartRequest(is_low_latency);
  if (ack != BluetoothAudioCtrlAck::PENDING) {
    auto aidl_retval =
        provider_->streamStarted(BluetoothAudioCtrlAckToHalStatus(ack));
    if (!aidl_retval.isOk()) {
      LOG(ERROR) << __func__ << ": BluetoothAudioHal failure: "
                 << aidl_retval.getDescription();
    }
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioPortImpl::suspendStream() {
  BluetoothAudioCtrlAck ack = transport_instance_->SuspendRequest();
  if (ack != BluetoothAudioCtrlAck::PENDING) {
    auto aidl_retval =
        provider_->streamSuspended(BluetoothAudioCtrlAckToHalStatus(ack));
    if (!aidl_retval.isOk()) {
      LOG(ERROR) << __func__ << ": BluetoothAudioHal failure: "
                 << aidl_retval.getDescription();
    }
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioPortImpl::stopStream() {
  transport_instance_->StopRequest();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioPortImpl::getPresentationPosition(
    PresentationPosition* _aidl_return) {
  uint64_t remote_delay_report_ns;
  uint64_t total_bytes_read;
  timespec data_position;
  bool retval = transport_instance_->GetPresentationPosition(
      &remote_delay_report_ns, &total_bytes_read, &data_position);

  PresentationPosition::TimeSpec transmittedOctetsTimeStamp;
  if (retval) {
    transmittedOctetsTimeStamp = timespec_convert_to_hal(data_position);
  } else {
    remote_delay_report_ns = 0;
    total_bytes_read = 0;
    transmittedOctetsTimeStamp = {};
  }
  LOG(INFO) << __func__ << ": result=" << retval
            << ", delay=" << remote_delay_report_ns
            << ", data=" << total_bytes_read
            << " byte(s), timestamp=" << transmittedOctetsTimeStamp.toString();
  _aidl_return->remoteDeviceAudioDelayNanos =
      static_cast<int64_t>(remote_delay_report_ns);
  _aidl_return->transmittedOctets = static_cast<int64_t>(total_bytes_read);
  _aidl_return->transmittedOctetsTimestamp = transmittedOctetsTimeStamp;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioPortImpl::updateSourceMetadata(
    const SourceMetadata& source_metadata) {
  LOG(INFO) << __func__ << ": " << source_metadata.tracks.size() << " track(s)";

  if (source_metadata.tracks.size() == 0) {
    LOG(INFO) << __func__ << ": Invalid Source metadata, return";
    return ndk::ScopedAStatus::ok();
  }

  std::vector<playback_track_metadata> metadata_vec;
  metadata_vec.reserve(source_metadata.tracks.size());
  for (const auto& metadata : source_metadata.tracks) {
    LOG(INFO) << __func__ << ": usage: " << static_cast<audio_usage_t>(metadata.usage);
    metadata_vec.push_back({
        .usage = static_cast<audio_usage_t>(metadata.usage),
        .content_type = static_cast<audio_content_type_t>(metadata.contentType),
        .gain = metadata.gain,
    });
  }
  const source_metadata_t legacy_source_metadata = {
      .track_count = metadata_vec.size(), .tracks = metadata_vec.data()};
  transport_instance_->SourceMetadataChanged(legacy_source_metadata);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioPortImpl::updateSinkMetadata(
    const SinkMetadata& sink_metadata) {
  LOG(INFO) << __func__ << ": " << sink_metadata.tracks.size() << " track(s)";

  if (sink_metadata.tracks.size() == 0) {
    LOG(INFO) << __func__ << ": Invalid Sink metadata, return";
    return ndk::ScopedAStatus::ok();
  }

  std::vector<record_track_metadata> metadata_vec;
  metadata_vec.reserve(sink_metadata.tracks.size());
  for (const auto& metadata : sink_metadata.tracks) {
    LOG(INFO) << __func__ << ": source: " << static_cast<audio_source_t>(metadata.source);
    metadata_vec.push_back({
        .source = static_cast<audio_source_t>(metadata.source),
        .gain = metadata.gain,
    });
  }
  const sink_metadata_t legacy_sink_metadata = {
      .track_count = metadata_vec.size(), .tracks = metadata_vec.data()};
  transport_instance_->SinkMetadataChanged(legacy_sink_metadata);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioPortImpl::setLatencyMode(
    LatencyMode latency_mode) {
  bool is_low_latency = latency_mode == LatencyMode::LOW_LATENCY ? true : false;
  return ndk::ScopedAStatus::ok();
}

PresentationPosition::TimeSpec BluetoothAudioPortImpl::timespec_convert_to_hal(
    const timespec& ts) {
  return {.tvSec = static_cast<int64_t>(ts.tv_sec),
          .tvNSec = static_cast<int64_t>(ts.tv_nsec)};
}

}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth