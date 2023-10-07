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
/* Changes from Qualcomm Innovation Center are provided under the following license:
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

#define LOG_TAG "BTAudioClientIf"

#include "client_interface.h"

#include <android/binder_manager.h>

namespace bluetooth {
namespace audio {
namespace aidl {

using ::aidl::android::hardware::bluetooth::audio::LeAudioConfiguration;
using ::aidl::android::hardware::bluetooth::audio::LeAudioCodecConfiguration;

std::unordered_set<BluetoothAudioClientInterface *> BluetoothAudioClientInterface::objs_address_;

std::ostream& operator<<(std::ostream& os, const BluetoothAudioCtrlAck& ack) {
  switch (ack) {
    case BluetoothAudioCtrlAck::SUCCESS_FINISHED:
      return os << "SUCCESS_FINISHED";
    case BluetoothAudioCtrlAck::PENDING:
      return os << "PENDING";
    case BluetoothAudioCtrlAck::FAILURE_UNSUPPORTED:
      return os << "FAILURE_UNSUPPORTED";
    case BluetoothAudioCtrlAck::FAILURE_BUSY:
      return os << "FAILURE_BUSY";
    case BluetoothAudioCtrlAck::FAILURE_DISCONNECTING:
      return os << "FAILURE_DISCONNECTING";
    case BluetoothAudioCtrlAck::FAILURE:
      return os << "FAILURE";
    default:
      return os << "UNDEFINED " << static_cast<int8_t>(ack);
  }
}

BluetoothAudioClientInterface::BluetoothAudioClientInterface(
    IBluetoothTransportInstance* instance)
    : provider_(nullptr),
      provider_factory_(nullptr),
      session_started_(false),
      data_mq_(nullptr),
      transport_(instance) {
  death_recipient_ = ::ndk::ScopedAIBinder_DeathRecipient(
      AIBinder_DeathRecipient_new(binderDiedCallbackAidl));

  objs_address_.insert(this);
}

BluetoothAudioClientInterface::~BluetoothAudioClientInterface() {
  objs_address_.erase(this);
}

bool BluetoothAudioClientInterface::is_aidl_available() {
  LOG(WARNING) << __func__ << ": aidl_available: " << aidl_available;
  if (!aidl_available) return false;
  auto service = AServiceManager_checkService(
                        kDefaultAudioProviderFactoryInterface.c_str());
  aidl_available = (service != nullptr);
  LOG(WARNING) << __func__
               << ": updating aidl_available: " << aidl_available;

  return aidl_available;
}

std::vector<AudioCapabilities>
BluetoothAudioClientInterface::GetAudioCapabilities() const {
  LOG(INFO) << __func__ << ": AIDL";
  return capabilities_;
}

std::vector<AudioCapabilities>
BluetoothAudioClientInterface::GetAudioCapabilities(SessionType session_type) {
  LOG(INFO) << __func__ << ": AIDL: " << static_cast<uint16_t>(session_type);
  std::vector<AudioCapabilities> capabilities(0);
  if (!is_aidl_available()) {
    return capabilities;
  }
  auto provider_factory = IBluetoothAudioProviderFactory::fromBinder(
      ::ndk::SpAIBinder(AServiceManager_getService(
          kDefaultAudioProviderFactoryInterface.c_str())));

  if (provider_factory == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: can't get capability from unknown factory";
    aidl_available = false;
    return capabilities;
  }

  auto aidl_retval =
      provider_factory->getProviderCapabilities(session_type, &capabilities);
  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__
               << ": AIDL: BluetoothAudioHal::getProviderCapabilities failure: "
               << aidl_retval.getDescription();
  }
  return capabilities;
}

void BluetoothAudioClientInterface::FetchAudioProvider() {
  LOG(INFO) << __func__;
  if (provider_ != nullptr) {
    LOG(WARNING) << __func__ << ": AIDL: refetch";
  } else if (!is_aidl_available()) {
    // AIDL availability should only be checked at the beginning.
    // When refetching, AIDL may not be ready *yet* but it's expected to be
    // available later.
    return;
  }
  auto provider_factory = IBluetoothAudioProviderFactory::fromBinder(
      ::ndk::SpAIBinder(AServiceManager_getService(
          kDefaultAudioProviderFactoryInterface.c_str())));

  if (provider_factory == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: can't get capability from unknown factory";
    aidl_available = false;
    return;
  }

  LOG(ERROR) << __func__ << ": AIDL: " << this;

  if (isDestroy(this)) {
    LOG(ERROR) << __func__ << ": AIDL: object deleted!";
    return;
  }

#if 0
  ndk::SpAIBinder provider_ext_binder;
  AIBinder_getExtension(
          ::ndk::SpAIBinder(AServiceManager_getService(kDefaultAudioProviderFactoryInterface.c_str())).get(),
          provider_ext_binder.getR());

  std::shared_ptr<IBluetoothAudioProviderExt> provider_ext =
         IBluetoothAudioProviderExt::fromBinder(provider_ext_binder);
#endif

  capabilities_.clear();
  auto aidl_retval = provider_factory->getProviderCapabilities(
      transport_->GetSessionType(), &capabilities_);
  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__
               << ": AIDL: BluetoothAudioHal::getProviderCapabilities failure: "
               << aidl_retval.getDescription();
    return;
  }
  if (capabilities_.empty()) {
    LOG(WARNING) << __func__
                 << ": AIDL: SessionType=" << toString(transport_->GetSessionType())
                 << " Not supported by BluetoothAudioHal";
    return;
  }
  LOG(INFO) << __func__ << ": AIDL: BluetoothAudioHal SessionType="
            << toString(transport_->GetSessionType()) << " has "
            << capabilities_.size() << " AudioCapabilities";

  aidl_retval =
      provider_factory->openProvider(transport_->GetSessionType(), &provider_);
  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal::openProvider failure: "
               << aidl_retval.getDescription();
  }
  CHECK(provider_ != nullptr);

  provider_factory_ = std::move(provider_factory);

  LOG(INFO) << ": AIDL: IBluetoothAudioProvidersFactory::openProvider() returned "
            << provider_.get()
            << (provider_->isRemote() ? " (remote)" : " (local)");
}

BluetoothAudioSinkClientInterface::BluetoothAudioSinkClientInterface(
    IBluetoothSinkTransportInstance* sink,
    thread_t* message_loop)
    : BluetoothAudioClientInterface{sink}, sink_(sink) {
  LOG(INFO) << __func__ << ": AIDL";
  FetchAudioProvider();
}

BluetoothAudioSinkClientInterface::~BluetoothAudioSinkClientInterface() {
  if (provider_factory_ != nullptr) {
    AIBinder_unlinkToDeath(provider_factory_->asBinder().get(), death_recipient_.get(),
                           nullptr);
  }
}

BluetoothAudioSourceClientInterface::BluetoothAudioSourceClientInterface(
    IBluetoothSourceTransportInstance* source,
    thread_t* message_loop)
    : BluetoothAudioClientInterface{source}, source_(source) {
  LOG(INFO) << __func__ << ": AIDL";
  FetchAudioProvider();
}

BluetoothAudioSourceClientInterface::~BluetoothAudioSourceClientInterface() {
  if (provider_factory_ != nullptr) {
    AIBinder_unlinkToDeath(provider_factory_->asBinder().get(), death_recipient_.get(),
                           nullptr);
  }
}

void BluetoothAudioClientInterface::binderDiedCallbackAidl(void* ptr) {
  LOG(WARNING) << __func__ << ": AIDL: restarting connection with new Audio Hal, ptr = " << ptr;
  auto client = static_cast<BluetoothAudioClientInterface*>(ptr);
  if (client == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: null audio HAL died!";
    return;
  }
  if (isDestroy(client)) {
    LOG(ERROR) << __func__ << "AIDL: object deleted!";
    return;
  }
  client->RenewAudioProviderAndSession();
}

bool BluetoothAudioClientInterface::UpdateAudioConfig(
    const AudioConfiguration& audio_config) {
  bool is_software_session =
      (transport_->GetSessionType() ==
           SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH ||
       transport_->GetSessionType() ==
           SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH ||
       transport_->GetSessionType() ==
           SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH ||
       transport_->GetSessionType() ==
           SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH ||
       transport_->GetSessionType() ==
           SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH);
  bool is_a2dp_offload_session =
      (transport_->GetSessionType() ==
       SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
  bool is_leaudio_offload_session =
      (transport_->GetSessionType() ==
           SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
       transport_->GetSessionType() ==
           SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH);
  bool is_leaudio_offload_broadcast_session =
      (transport_->GetSessionType() ==
           SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
  auto audio_config_tag = audio_config.getTag();

  LOG(ERROR) << __func__
             << ": AIDL: is_software_session: " << is_software_session
             << ", is_a2dp_offload_session: " << is_a2dp_offload_session
             << ", is_leaudio_offload_session: " << is_leaudio_offload_session;

  //LOG(ERROR) << __func__
  //             << ": audio_config_tag: " << audio_config_tag;

  bool is_software_audio_config =
      (is_software_session &&
       audio_config_tag == AudioConfiguration::pcmConfig);

  bool is_a2dp_offload_audio_config =
      (is_a2dp_offload_session &&
       audio_config_tag == AudioConfiguration::a2dpConfig);

  bool is_leaudio_offload_audio_config =
      (is_leaudio_offload_session &&
       audio_config_tag == AudioConfiguration::leAudioConfig);

  bool is_leaudio_offload_broadcast_audio_config =
      (is_leaudio_offload_broadcast_session &&
       audio_config_tag == AudioConfiguration::leAudioBroadcastConfig);

  LOG(ERROR) << __func__
        << ": AIDL: is_software_audio_config: " << is_software_audio_config
        << ", is_a2dp_offload_audio_config: " << is_a2dp_offload_audio_config
        << ", is_leaudio_offload_audio_config: " << is_leaudio_offload_audio_config
        << ", is_leaudio_offload_broadcast_session: " << is_leaudio_offload_broadcast_audio_config;

  if (!is_software_audio_config && !is_a2dp_offload_audio_config &&
      !is_leaudio_offload_audio_config && !is_leaudio_offload_broadcast_audio_config) {
    return false;
  }

  if (is_leaudio_offload_audio_config) {
    const LeAudioConfiguration *leAudioConfig =
	     (&audio_config.get<AudioConfiguration::leAudioConfig>());
  }

  if (is_leaudio_offload_broadcast_audio_config) {
    const LeAudioBroadcastConfiguration *leBroadcastAudioConfig = &audio_config.get<AudioConfiguration::leAudioBroadcastConfig>();
    LOG(ERROR) << __func__
        << " broadcast_audio_config: codec type: " << (int)leBroadcastAudioConfig->codecType
        << " samplingFrequencyHz: "
        << leBroadcastAudioConfig->streamMap[0].leAudioCodecConfig.get<LeAudioCodecConfiguration::lc3Config>().samplingFrequencyHz
        << " octetsPerFrame: "
        << leBroadcastAudioConfig->streamMap[0].leAudioCodecConfig.get<LeAudioCodecConfiguration::lc3Config>().octetsPerFrame
        << " frameDurationUs: "
        << leBroadcastAudioConfig->streamMap[0].leAudioCodecConfig.get<LeAudioCodecConfiguration::lc3Config>().frameDurationUs
        << " pcmBitDepth: "
        << leBroadcastAudioConfig->streamMap[0].leAudioCodecConfig.get<LeAudioCodecConfiguration::lc3Config>().pcmBitDepth
        << " blocksPerSdu: "
        << leBroadcastAudioConfig->streamMap[0].leAudioCodecConfig.get<LeAudioCodecConfiguration::lc3Config>().blocksPerSdu
        << " channelMode: "
        << (int)leBroadcastAudioConfig->streamMap[0].leAudioCodecConfig.get<LeAudioCodecConfiguration::lc3Config>().channelMode;
  }

  transport_->UpdateAudioConfiguration(audio_config);

  if (provider_ == nullptr) {
    LOG(INFO) << __func__
              << ": AIDL: BluetoothAudioHal nullptr, update it as session started";
    return true;
  }

  auto aidl_retval = provider_->updateAudioConfiguration(audio_config);
  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal failure: "
               << aidl_retval.getDescription();
  }
  return true;
}

int BluetoothAudioClientInterface::StartSession() {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  LOG(INFO) << __func__ << ": session_started_: " << session_started_;
  if (provider_ == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal nullptr";
    session_started_ = false;
    return -EINVAL;
  }
  if (session_started_) {
    LOG(ERROR) << __func__ << ": AIDL: session started already";
    return -EBUSY;
  }

  std::shared_ptr<IBluetoothAudioPort> stack_if =
      ndk::SharedRefBase::make<BluetoothAudioPortImpl>(transport_, provider_);

  std::unique_ptr<DataMQ> data_mq;
  DataMQDesc mq_desc;

  std::vector<LatencyMode> latency_modes = {LatencyMode::FREE};

  binder_status_t binder_status = AIBinder_linkToDeath(
     provider_factory_->asBinder().get(), death_recipient_.get(), this);
  if (binder_status != STATUS_OK) {
    LOG(ERROR) << ": Failed to linkToDeath " << static_cast<int>(binder_status);
  }

  auto aidl_retval = provider_->startSession(
      stack_if, transport_->GetAudioConfiguration(), latency_modes, &mq_desc);
  if (!aidl_retval.isOk()) {
    if (aidl_retval.getExceptionCode() == EX_ILLEGAL_ARGUMENT) {
      LOG(ERROR) << __func__ << ": BluetoothAudioHal Error: "
                 << aidl_retval.getDescription() << ", audioConfig="
                 << transport_->GetAudioConfiguration().toString();
    } else {
      LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal failure: "
               << aidl_retval.getDescription();
    }
    return -EPROTO;
  }
  data_mq.reset(new DataMQ(mq_desc));

  if (data_mq && data_mq->isValid()) {
    data_mq_ = std::move(data_mq);
  } else if (transport_->GetSessionType() ==
                 SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
             transport_->GetSessionType() ==
                 SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH ||
             transport_->GetSessionType() ==
                 SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
             transport_->GetSessionType() ==
                 SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH) {
    transport_->ResetPresentationPosition();
    session_started_ = true;
    return 0;
  }
  if (data_mq_ && data_mq_->isValid()) {
    transport_->ResetPresentationPosition();
    session_started_ = true;
    return 0;
  } else {
    if (!data_mq_) {
      LOG(ERROR) << __func__ << ": AIDL: Failed to obtain audio data path";
    }
    if (data_mq_ && !data_mq_->isValid()) {
      LOG(ERROR) << __func__ << ": AIDL: Audio data path is invalid";
    }
    session_started_ = false;
    return -EIO;
  }
}

void BluetoothAudioClientInterface::StreamStarted(
    const BluetoothAudioCtrlAck& ack) {
  LOG(INFO) << __func__ << ": AIDL";
  if (provider_ == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal nullptr";
    return;
  }
  if (ack == BluetoothAudioCtrlAck::PENDING) {
    LOG(INFO) << __func__ << ": AIDL: " << ack << " ignored";
    return;
  }
  BluetoothAudioStatus status = BluetoothAudioCtrlAckToHalStatus(ack);

  auto aidl_retval = provider_->streamStarted(status);

  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal failure: "
               << aidl_retval.getDescription();
  }
}

void BluetoothAudioClientInterface::StreamSuspended(
                             const BluetoothAudioCtrlAck& ack) {
  LOG(INFO) << __func__ << ": AIDL";
  if (provider_ == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal nullptr";
    return;
  }
  if (ack == BluetoothAudioCtrlAck::PENDING) {
    LOG(INFO) << __func__ << ": AIDL: " << ack << " ignored";
    return;
  }
  BluetoothAudioStatus status = BluetoothAudioCtrlAckToHalStatus(ack);

  auto aidl_retval = provider_->streamSuspended(status);

  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal failure: "
               << aidl_retval.getDescription();
  }
}

int BluetoothAudioClientInterface::EndSession() {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  LOG(INFO) << __func__ << ": session_started_: " << session_started_;
  if (!session_started_) {
    LOG(INFO) << __func__ << ": AIDL: session ended already";
    return 0;
  }

  session_started_ = false;
  if (provider_ == nullptr) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal nullptr";
    return -EINVAL;
  }
  data_mq_ = nullptr;

  auto aidl_retval = provider_->endSession();

  if (!aidl_retval.isOk()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal failure: "
               << aidl_retval.getDescription();
    return -EPROTO;
  }

  if (provider_factory_ != nullptr) {
    binder_status_t binder_status = AIBinder_unlinkToDeath(provider_factory_->asBinder().get(),
                                                           death_recipient_.get(), this);
    if (binder_status == STATUS_OK) {
       LOG(ERROR) << __func__ << ": AIBinder_unlinkToDeath success";
    }
  }

  return 0;
}

void BluetoothAudioClientInterface::FlushAudioData() {
  LOG(INFO) << __func__ << ": AIDL";
  if (transport_->GetSessionType() ==
          SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
      transport_->GetSessionType() ==
          SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH ||
      transport_->GetSessionType() ==
          SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH) {
    return;
  }

  if (data_mq_ == nullptr || !data_mq_->isValid()) {
    LOG(WARNING) << __func__ << ": AIDL: data_mq_ invalid";
    return;
  }
  size_t size = data_mq_->availableToRead();
  std::vector<MqDataType> buffer(size);

  if (data_mq_->read(buffer.data(), size) != size) {
    LOG(WARNING) << __func__ << ": AIDL: failed to flush data queue!";
  }
}

size_t BluetoothAudioSinkClientInterface::ReadAudioData(uint8_t* p_buf,
                                                        uint32_t len) {
  LOG(INFO) << __func__ << ": AIDL";
  if (!IsValid()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal is not valid";
    return 0;
  }
  if (p_buf == nullptr || len == 0) return 0;

  std::lock_guard<std::mutex> guard(internal_mutex_);

  size_t total_read = 0;
  int timeout_ms = kDefaultDataReadTimeoutMs;
  do {
    if (data_mq_ == nullptr || !data_mq_->isValid()) break;

    size_t avail_to_read = data_mq_->availableToRead();
    if (avail_to_read) {
      if (avail_to_read > len - total_read) {
        avail_to_read = len - total_read;
      }
      if (data_mq_->read((MqDataType*)p_buf + total_read, avail_to_read) == 0) {
        LOG(WARNING) << __func__ << ": AIDL: len=" << len
                     << " total_read=" << total_read << " failed";
        break;
      }
      total_read += avail_to_read;
    } else if (timeout_ms >= kDefaultDataReadPollIntervalMs) {
     /*std::this_thread::sleep_for(
          std::chrono::milliseconds(kDefaultDataWritePollIntervalMs));*/
      timeout_ms -= kDefaultDataReadPollIntervalMs;
      continue;
    } else {
      LOG(WARNING) << __func__ << ": AIDL: " << (len - total_read) << "/" << len
                   << " no data " << (kDefaultDataReadTimeoutMs - timeout_ms)
                   << " ms";
      break;
    }
  } while (total_read < len);

  if (timeout_ms <
          (kDefaultDataReadTimeoutMs - kDefaultDataReadPollIntervalMs) &&
      timeout_ms >= kDefaultDataReadPollIntervalMs) {
    VLOG(1) << __func__ << ": AIDL: underflow " << len << " -> " << total_read
            << " read " << (kDefaultDataReadTimeoutMs - timeout_ms) << " ms";
  } else {
    VLOG(2) << __func__ << ": " << len << " -> " << total_read << " read";
  }

  sink_->LogBytesRead(total_read);
  return total_read;
}

void BluetoothAudioClientInterface::RenewAudioProviderAndSession() {
  // NOTE: must be invoked on the same thread where this
  // BluetoothAudioClientInterface is running
  LOG(INFO) << __func__ << ": session_started_: " << session_started_;
  uint8_t retries = 0;
  while(retries < 10) {
    LOG(INFO) << __func__ << "AIDL: sleep for 50ms for hal server to restart";
    auto service = AServiceManager_checkService(
                          kDefaultAudioProviderFactoryInterface.c_str());
    if(service != nullptr)
      break;
    usleep(50000);
    retries++;
  }
  FetchAudioProvider();

  if (session_started_) {
    LOG(INFO) << __func__
              << ": AIDL: Restart the session while audio HAL recovering ";
    session_started_ = false;

    StartSession();
  }
}

size_t BluetoothAudioSourceClientInterface::WriteAudioData(const uint8_t* p_buf,
                                                           uint32_t len) {
  LOG(INFO) << __func__ << ": AIDL";
  if (!IsValid()) {
    LOG(ERROR) << __func__ << ": AIDL: BluetoothAudioHal is not valid";
    return 0;
  }
  if (p_buf == nullptr || len == 0) return 0;

  std::lock_guard<std::mutex> guard(internal_mutex_);

  size_t total_written = 0;
  int timeout_ms = kDefaultDataWriteTimeoutMs;
  do {
    if (data_mq_ == nullptr || !data_mq_->isValid()) break;

    size_t avail_to_write = data_mq_->availableToWrite();
    if (avail_to_write) {
      if (avail_to_write > len - total_written) {
        avail_to_write = len - total_written;
      }
      if (data_mq_->write((const MqDataType*)p_buf + total_written,
                          avail_to_write) == 0) {
        LOG(WARNING) << __func__ << ": AIDL: len=" << len
                     << " total_written=" << total_written << " failed";
        break;
      }
      total_written += avail_to_write;
    } else if (timeout_ms >= kDefaultDataWritePollIntervalMs) {
      /*std::this_thread::sleep_for(
          std::chrono::milliseconds(kDefaultDataWritePollIntervalMs));*/
      timeout_ms -= kDefaultDataWritePollIntervalMs;
      continue;
    } else {
      LOG(WARNING) << __func__ << ": AIDL: " << (len - total_written) << "/" << len
                   << " no data " << (kDefaultDataWriteTimeoutMs - timeout_ms)
                   << " ms";
      break;
    }
  } while (total_written < len);

  if (timeout_ms <
          (kDefaultDataWriteTimeoutMs - kDefaultDataWritePollIntervalMs) &&
      timeout_ms >= kDefaultDataWritePollIntervalMs) {
    VLOG(1) << __func__ << ": AIDL: underflow " << len << " -> " << total_written
            << " read " << (kDefaultDataWriteTimeoutMs - timeout_ms) << " ms ";
  } else {
    VLOG(2) << __func__ << ": AIDL: " << len << " -> " << total_written
            << " written ";
  }

  source_->LogBytesWritten(total_written);
  return total_written;
}

}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth
