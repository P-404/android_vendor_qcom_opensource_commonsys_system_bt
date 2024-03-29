/******************************************************************************
 *
 *  Copyright (C) 2017  The Android Open Source Project
 *  Copyright (C) 2014  Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *  Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *  SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 ******************************************************************************/

#include "bt_target.h"
#include "device/include/controller.h"
#include "osi/include/alarm.h"

#include "stack_config.h"
#include "ble_advertiser.h"
#include "ble_advertiser_hci_interface.h"
#include "btm_int.h"
#include "btm_int_types.h"
#include "stack/btm/btm_ble_int.h"

#include <string.h>
#include <string>
#include <queue>
#include <vector>

#include <openssl/aead.h>
#include <openssl/base.h>
#include <openssl/rand.h>
#include "hcimsgs.h"
#include "gap_api.h"

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <base/location.h>
#include <base/logging.h>
#include <base/memory/weak_ptr.h>
#include <base/strings/string_number_conversions.h>
#include <base/time/time.h>

using base::Bind;
using base::TimeDelta;
using base::TimeTicks;
using RegisterCb =
    base::Callback<void(uint8_t /* inst_id */, uint8_t /* status */)>;
using IdTxPowerStatusCb = base::Callback<void(
    uint8_t /* inst_id */, int8_t /* tx_power */, uint8_t /* status */)>;
using CreateBIGCb = base::Callback<void(uint8_t /*adv_inst_id*/, uint8_t /*status*/,
      uint8_t /*big_handle*/, uint32_t /*big_sync_delay*/,
      uint32_t /*transport_latency_big*/, uint8_t /*phy*/, uint8_t /*nse*/,
      uint8_t /*bn*/, uint8_t /*pto*/, uint8_t /*irc*/, uint16_t /*max_pdu*/,
      uint16_t /*iso_int*/, uint8_t /*num_bis*/,
      std::vector<uint16_t> /*conn_handle_list*/)>;
using TerminateBIGCb =
    base::Callback<void(uint8_t /* status */, uint8_t /* adv_inst_id */,
                        uint8_t /* big_handle */, uint8_t /* status */)>;
using SetEnableData = BleAdvertiserHciInterface::SetEnableData;
extern void btm_gen_resolvable_private_addr(
    base::Callback<void(const RawAddress& rpa)> cb);
std::mutex lock_;
constexpr int EXT_ADV_DATA_LEN_MAX = 251;
constexpr int PERIODIC_ADV_DATA_LEN_MAX = 252;

#define ADVERTISE_FAILED_FEATURE_UNSUPPORTED 0x05

namespace {

bool is_connectable(uint16_t advertising_event_properties) {
  return advertising_event_properties & 0x01;
}

struct IsoBIGInstance {
  uint8_t big_handle;
  bool in_use;
  std::vector<uint16_t> bis_handles;
  uint8_t adv_inst_id;
  bool created_status;
  CreateBIGCb create_big_cb;
  TerminateBIGCb terminate_big_cb;

  bool IsCreated() { return created_status; }

  IsoBIGInstance(int big_handle)
      : big_handle(big_handle),
        in_use(false),
        created_status(false) {
  }

  ~IsoBIGInstance() {
  }
};

struct AdvertisingInstance {
  uint8_t inst_id;
  bool in_use;
  uint8_t advertising_event_properties;
  alarm_t* adv_raddr_timer;
  int8_t tx_power;
  uint16_t duration;  // 1 unit is 10ms
  uint8_t maxExtAdvEvents;
  alarm_t* timeout_timer;
  uint8_t own_address_type;
  RawAddress own_address;
  MultiAdvCb timeout_cb;
  bool address_update_required;
  bool periodic_enabled;
  uint32_t advertising_interval;  // 1 unit is 0.625 ms
  uint8_t skip_rpa_count;
  bool skip_rpa;
  std::array<uint8_t,5> randomizer;
  std::vector<uint8_t> advertise_data;
  std::vector<uint8_t> scan_response_data;
  std::vector<uint8_t> periodic_data;
  std::vector<uint8_t> advertise_data_enc;
  std::vector<uint8_t> scan_response_data_enc;
  std::vector<uint8_t> periodic_adv_data_enc;
  std::vector<uint8_t> enc_key_value;
  /* When true, advertising set is enabled, or last scheduled call to "LE Set
   * Extended Advertising Set Enable" is to enable this advertising set. Any
   * command scheduled when in this state will execute when the set is enabled,
   * unless enabling fails.
   *
   * When false, advertising set is disabled, or last scheduled call to "LE Set
   * Extended Advertising Set Enable" is to disable this advertising set. Any
   * command scheduled when in this state will execute when the set is disabled.
   */
  bool enable_status;
  TimeTicks enable_time;

  uint8_t big_handle;

  bool IsEnabled() { return enable_status; }

  bool IsConnectable() { return is_connectable(advertising_event_properties); }

  AdvertisingInstance(int inst_id)
      : inst_id(inst_id),
        in_use(false),
        advertising_event_properties(0),
        tx_power(0),
        duration(0),
        timeout_timer(nullptr),
        own_address_type(0),
        own_address(RawAddress::kEmpty),
        address_update_required(false),
        periodic_enabled(false),
        skip_rpa_count(0),
        skip_rpa(false),
        enable_status(false),
        big_handle(INVALID_BIG_HANDLE) {
    adv_raddr_timer = alarm_new_periodic("btm_ble.adv_raddr_timer");
  }

  ~AdvertisingInstance() {
    in_use = false;
    alarm_free(adv_raddr_timer);
    adv_raddr_timer = nullptr;
    if (timeout_timer) {
      alarm_free(timeout_timer);
      timeout_timer = nullptr;
    }
  }
};

void btm_ble_adv_raddr_timer_timeout(void* data);

struct closure_data {
  base::Closure user_task;
  base::Location posted_from;
};

static void alarm_closure_cb(void* p) {
  closure_data* data = (closure_data*)p;
  VLOG(1) << "executing timer scheduled at %s" << data->posted_from.ToString();
  data->user_task.Run();
  delete data;
}

// Periodic alarms are not supported, because we clean up data in callback
void alarm_set_closure(const base::Location& posted_from, alarm_t* alarm,
                       uint64_t interval_ms, base::Closure user_task) {
  closure_data* data = new closure_data;
  data->posted_from = posted_from;
  data->user_task = std::move(user_task);
  VLOG(1) << "scheduling timer %s" << data->posted_from.ToString();
  alarm_set_on_mloop(alarm, interval_ms, alarm_closure_cb, data);
}

class BleAdvertisingManagerImpl;

/* a temporary type for holding all the data needed in callbacks below*/
struct CreatorParams {
  uint8_t inst_id;
  base::WeakPtr<BleAdvertisingManagerImpl> self;
  IdTxPowerStatusCb cb;
  tBTM_BLE_ADV_PARAMS params;
  std::vector<uint8_t> advertise_data;
  std::vector<uint8_t> advertise_data_enc;
  std::vector<uint8_t> scan_response_data;
  std::vector<uint8_t> scan_response_data_enc;
  tBLE_PERIODIC_ADV_PARAMS periodic_params;
  std::vector<uint8_t> periodic_data;
  std::vector<uint8_t> periodic_adv_data_enc;
  std::vector<uint8_t> enc_key_value;
  uint16_t duration;
  uint8_t maxExtAdvEvents;
  tBLE_CREATE_BIG_PARAMS create_big_params;
  RegisterCb timeout_cb;
};

void GenerateRandomizer_cmpl(AdvertisingInstance* p_inst, uint16_t temp_randomizer[5],
                            GenerateRandomizerCb cb){
  memcpy(p_inst->randomizer.data(), temp_randomizer, 5);
  std::reverse(p_inst->randomizer.begin(),p_inst->randomizer.end());
  cb.Run(BTM_BLE_MULTI_ADV_SUCCESS);
}

using c_type = std::unique_ptr<CreatorParams>;

BleAdvertisingManager* instance;
base::WeakPtr<BleAdvertisingManagerImpl> instance_weakptr;

class BleAdvertisingManagerImpl
    : public BleAdvertisingManager,
      public BleAdvertiserHciInterface::AdvertisingEventObserver {
 public:
  BleAdvertisingManagerImpl(BleAdvertiserHciInterface* interface)
      : hci_interface(interface), weak_factory_(this) {
    hci_interface->ReadInstanceCount(
        base::Bind(&BleAdvertisingManagerImpl::ReadInstanceCountCb,
                   weak_factory_.GetWeakPtr()));
  }

  ~BleAdvertisingManagerImpl() { adv_inst.clear(); }

  void GetOwnAddress(uint8_t inst_id, GetAddressCallback cb) override {
    cb.Run(adv_inst[inst_id].own_address_type, adv_inst[inst_id].own_address);
  }

  void ReadInstanceCountCb(uint8_t instance_count) {
    this->inst_count = instance_count;
    adv_inst.reserve(inst_count);
    /* Initialize adv instance indices and IDs. */
    for (uint8_t i = 0; i < inst_count; i++) {
      adv_inst.emplace_back(i);
    }

    //ISO BIG
    iso_big_inst.reserve(inst_count);
    /* Initialize big instance indices and IDs. */
    for (uint8_t i = 0; i < inst_count; i++) {
      iso_big_inst.emplace_back(i);
    }
  }

  std::vector<uint8_t> EncryptedAdvertising(AdvertisingInstance* p_inst,
                                            std::vector<uint8_t> data) {
    std::vector<uint8_t> ED_AD_Data; /*Randomizer + Payload + Out_Tag(MIC)*/
    std::vector<uint8_t> key;
    std::vector<uint8_t> iv;
    if (p_inst->enc_key_value.empty()) { /* Check to see if we have a user provided Key & IV*/
      if (btm_cb.enc_adv_data_log_enabled) {
        VLOG(1) << __func__ << " Gap Key";
      }
      tGAP_BLE_ATTR_VALUE temp = GAP_BleReadEncrKeyMaterial();
      for (unsigned int i = 0; i < 16; i++) {
        uint8_t temp_key = temp.encr_material.session_key[i];
        uint8_t temp_iv;
        key.push_back(temp_key);
        if(i < 8){
          temp_iv = temp.encr_material.init_vector[i];
          iv.push_back(temp_iv);
        }
      }
    }
    else {
      if (btm_cb.enc_adv_data_log_enabled) {
        VLOG(1) << __func__ << " User Key";
      }
      for (unsigned int i = 0; i < 24; i++) {
        if ( i < 16 ) {
          key.push_back(p_inst->enc_key_value[i]);
        }
        else {
          iv.push_back(p_inst->enc_key_value[i]);
        }
      }
    }
    std::vector<uint8_t> nonce;
    nonce.insert(nonce.end(), p_inst->randomizer.rbegin(), p_inst->randomizer.rend());
    nonce.insert(nonce.end(), iv.rbegin(), iv.rend());
    std::vector<uint8_t> in = data;
    static const std::vector<uint8_t> ad = {0xEA};
    std::vector<uint8_t> out(in.size());
    const EVP_AEAD *ccm_instance = EVP_aead_aes_128_ccm_bluetooth();
    const EVP_AEAD_CTX *aeadCTX = EVP_AEAD_CTX_new(ccm_instance, key.data(), key.size(),
                                                  EVP_AEAD_DEFAULT_TAG_LENGTH);
    if (aeadCTX == nullptr) return ED_AD_Data;
    size_t out_tag_len;
    std::vector<uint8_t> out_tag(EVP_AEAD_max_overhead(ccm_instance));
    if (btm_cb.enc_adv_data_log_enabled) {
      if (!key.empty()) {
        VLOG(1) << "Encr Data Key Material (Key): " << base::HexEncode(key.data(),key.size());
      }
      if (!iv.empty()) {
        VLOG(1) << "Encr Data Key Material (IV): " << base::HexEncode(iv.data(),iv.size());
      }
      VLOG(1) << "Randomizer: " << base::HexEncode(p_inst->randomizer.data(),
                                                    p_inst->randomizer.size());
      VLOG(1) << "Input: " << base::HexEncode(in.data(), in.size());
      VLOG(1) << "Nonce: " << base::HexEncode(nonce.data(), nonce.size());
      VLOG(1) << "Input AD: " << base::HexEncode(ad.data(), ad.size());
    }
    /* Function below encrypts the Input (From BoringSSL) */
    int result = EVP_AEAD_CTX_seal_scatter(aeadCTX, out.data(), out_tag.data(), &out_tag_len,
                                          out_tag.size(), nonce.data(), nonce.size(), in.data(),
                                          in.size(), nullptr, 0, ad.data(), ad.size());
    if (btm_cb.enc_adv_data_log_enabled) {
      VLOG(1) << "Out: " << base::HexEncode(out.data(), out.size());
      VLOG(1) << "MIC: " << base::HexEncode(out_tag.data(), out_tag.size());
    }
    ED_AD_Data.insert(ED_AD_Data.end(), p_inst->randomizer.rbegin(), p_inst->randomizer.rend());
    ED_AD_Data.insert(ED_AD_Data.end(), out.begin(), out.end());
    ED_AD_Data.insert(ED_AD_Data.end(), out_tag.begin(), out_tag.end());
    if (btm_cb.enc_adv_data_log_enabled) {
      VLOG(1) << "ED AD Data: " << base::HexEncode(ED_AD_Data.data(), ED_AD_Data.size());
    }
    /* Below we are forming the LTV for Encrypted Data */
    ED_AD_Data.insert(ED_AD_Data.begin(), BTM_BLE_AD_TYPE_ED);
    ED_AD_Data.insert(ED_AD_Data.begin(), ED_AD_Data.size());
    return ED_AD_Data;
  }

  void GenerateRandomizer(AdvertisingInstance* p_inst, GenerateRandomizerCb cb) {
    btsnd_hcic_ble_rand(
      Bind([](AdvertisingInstance* p_inst, GenerateRandomizerCb cb, BT_OCTET8 rand) {
          uint16_t randomizer[5];
          memcpy(randomizer, rand, 5);
          GenerateRandomizer_cmpl(p_inst, randomizer, cb);
      },
      p_inst, cb));
  }

  void GenerateRpa(base::Callback<void(const RawAddress&)> cb) {
    btm_gen_resolvable_private_addr(std::move(cb));
  }

  void AdvertiseRestart(bool restart, bool enable, AdvertisingInstance *p_inst,
                                    BleAdvertiserHciInterface* hci_interface) {
    VLOG(1) << __func__ << " enable: " << enable;
    if (restart) {
      if (!enable) {
        p_inst->enable_status = false;
        hci_interface->Enable(false, p_inst->inst_id, 0x00, 0x00,
                              base::DoNothing());
      }
      else {
        p_inst->enable_status = true;
        hci_interface->Enable(true, p_inst->inst_id, 0x00, 0x00,
                              base::DoNothing());
      }
    }
  }

  void ConfigureRpa(AdvertisingInstance* p_inst, MultiAdvCb configuredCb) {
    /* Connectable advertising set must be disabled when updating RPA */
    bool restart = p_inst->IsEnabled() && p_inst->IsConnectable();

    if (p_inst->skip_rpa) {
      if (p_inst->skip_rpa_count > 0) {
        p_inst->skip_rpa_count--;
        return;
      } else {
        VLOG(1) << __func__ << ": Set skip_rpa_count for broadcast";
        p_inst->skip_rpa_count = 15;
      }
    }
    // If there is any form of timeout on the set, schedule address update when
    // the set stops, because there is no good way to compute new timeout value.
    // Maximum duration value is around 10 minutes, so this is safe.
    if (restart && (p_inst->duration || p_inst->maxExtAdvEvents)) {
      p_inst->address_update_required = true;
      configuredCb.Run(0x01);
      return;
    }

    GenerateRpa(Bind(
        [](AdvertisingInstance* p_inst, MultiAdvCb configuredCb,
           const RawAddress& bda) {
          /* Connectable advertising set must be disabled when updating RPA */
          bool restart = p_inst->IsEnabled() && p_inst->IsConnectable();
          /* This check below ensures that advertising is restarted regardless of connectability*/
          if (!p_inst->advertise_data_enc.empty() || !p_inst->scan_response_data_enc.empty()
              || !p_inst->periodic_adv_data_enc.empty()) {
            restart = true;
          }

          if (!instance_weakptr.get()) return;
          auto hci_interface = instance_weakptr.get()->GetHciInterface();
          BleAdvertisingManagerImpl *ptr = instance_weakptr.get();
          if (ptr == nullptr) return;

          ptr->AdvertiseRestart(restart, false, p_inst, hci_interface);

          p_inst->own_address = bda;
          /* set it to controller */
          hci_interface->SetRandomAddress(
              p_inst->inst_id, bda,
              Bind(
                   [](AdvertisingInstance* p_inst,
                     MultiAdvCb configuredCb, uint8_t status) {
                    configuredCb.Run(0x00);
                  },
                  p_inst, configuredCb));
          /*This covers the Security Requirement
          of Generating a new Randomizer when the RPA changes.
          The if block below checks for if Advertising Data includes Encrypted Data.
          If it does we then call SetData which generates a new Randomizer */
          if(!p_inst->advertise_data_enc.empty()) {
            if (btm_cb.enc_adv_data_log_enabled) {
              VLOG(1) << __func__ << "ConfigureRPA - Encrypted Advertising";
            }
            ptr->SetData(p_inst->inst_id, false, p_inst->advertise_data, p_inst->advertise_data_enc,
                Bind(
                  [](AdvertisingInstance *p_inst, bool restart,
                    BleAdvertiserHciInterface* hci_interface,
                    BleAdvertisingManagerImpl *ptr, MultiAdvCb configuredCb, uint8_t status) {
                    if (status != 0){
                      LOG(ERROR) << "Set Data Failed: " << +status;
                      configuredCb.Run(status);
                      return;
                    }
                    /* This SetData below will result in a new Randomizer to be
                    generated as long the Scan Response Data also includes Encrypted Data.
                    If it does not then a new Randomizer will not be generated */
                    ptr->SetData(p_inst->inst_id, true, p_inst->scan_response_data,
                        p_inst->scan_response_data_enc,
                            Bind(
                              [](AdvertisingInstance *p_inst, bool restart,
                                BleAdvertiserHciInterface* hci_interface,
                                BleAdvertisingManagerImpl *ptr, MultiAdvCb configuredCb,
                                uint8_t status) {
                                if (status != 0){
                                  LOG(ERROR) << "Set Scan Response Data Failed: " << +status;
                                  configuredCb.Run(status);
                                  return;
                                }
                                  /* The if block below will run if periodic advertising data also
                                  includes encrypted data*/
                                if (!p_inst->periodic_adv_data_enc.empty() &&
                                    p_inst->periodic_enabled) {
                                  if (btm_cb.enc_adv_data_log_enabled) {
                                    VLOG(1) << "ConfigureRPA - Periodic Encrypted Data Exists";
                                  }
                                  ptr->SetPeriodicAdvertisingData(p_inst->inst_id,
                                      p_inst->periodic_data, p_inst->periodic_adv_data_enc,
                                          Bind(
                                            [](AdvertisingInstance *p_inst, bool restart,
                                              BleAdvertiserHciInterface* hci_interface,
                                              BleAdvertisingManagerImpl *ptr,
                                              MultiAdvCb configuredCb, uint8_t status) {
                                              if (status != 0) {
                                                LOG(ERROR) << "Set Periodic Data Failed: "
                                                    << +status;
                                                configuredCb.Run(status);
                                                return;
                                              }
                                              ptr->AdvertiseRestart(restart,true,p_inst,
                                                                    hci_interface);
                                            },
                                            p_inst, restart, hci_interface, ptr,
                                            std::move(configuredCb)));
                                } else {
                                  ptr->AdvertiseRestart(restart,true,p_inst, hci_interface);
                                }
                              },
                              p_inst, restart, hci_interface, ptr, std::move(configuredCb)));
                  },
                  p_inst, restart, hci_interface, ptr, std::move(configuredCb)));

          } else if (!p_inst->scan_response_data_enc.empty()) {
            if (btm_cb.enc_adv_data_log_enabled) {
              VLOG(1) << __func__ << " Scan Response Encrypted Data Exists";
            }
            ptr->SetData(p_inst->inst_id, true, p_inst->scan_response_data,
              p_inst->scan_response_data_enc,
                  Bind(
                    [](AdvertisingInstance *p_inst, bool restart,
                      BleAdvertiserHciInterface* hci_interface,
                      BleAdvertisingManagerImpl *ptr, MultiAdvCb configuredCb,
                      uint8_t status) {
                      if (status != 0){
                        LOG(ERROR) << "Set Scan Response Data Failed: " << +status;
                        configuredCb.Run(status);
                        return;
                      }
                      ptr->AdvertiseRestart(restart,true,p_inst, hci_interface);
                    },
                    p_inst, restart, hci_interface, ptr, std::move(configuredCb)));
          }
          /* This else if block handles the case in the scenario where
          Advertising Data does not include encrypted data,but periodic
          advertising data does include encrypted advertising data */
          else if ((!p_inst->periodic_adv_data_enc.empty() && p_inst->periodic_enabled) &&
                p_inst->advertise_data_enc.empty() && p_inst->scan_response_data_enc.empty()) {
                  if (btm_cb.enc_adv_data_log_enabled) {
                    VLOG(1) << "ConfigureRPA - Periodic Encrypted Data Exists";
                  }
                  ptr->SetPeriodicAdvertisingData(p_inst->inst_id, p_inst->periodic_data,
                      p_inst->periodic_adv_data_enc,
                        Bind(
                          [](AdvertisingInstance *p_inst, bool restart,
                            BleAdvertiserHciInterface* hci_interface,
                            BleAdvertisingManagerImpl *ptr, MultiAdvCb configuredCb,
                            uint8_t status) {
                            if (status != 0) {
                              LOG(ERROR) << "Set Periodic Data Failed: " << +status;
                              configuredCb.Run(status);
                              return;
                            }
                            ptr->AdvertiseRestart(restart, true, p_inst, hci_interface);
                          },
                          p_inst, restart, hci_interface, ptr, std::move(configuredCb)));
          } else {
            ptr->AdvertiseRestart(restart, true, p_inst, hci_interface);
          }
        },
        p_inst, std::move(configuredCb)));
  }

  void RegisterAdvertiser(
      base::Callback<void(uint8_t /* inst_id */, uint8_t /* status */)> cb)
      override {
    int own_address_type =
        BTM_BleLocalPrivacyEnabled() ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    RegisterAdvertiserImpl(own_address_type, cb);
  }

  void RegisterAdvertiserImpl(
      int own_address_type,
      base::Callback<void(uint8_t /* inst_id */, uint8_t /* status */)> cb) {
    AdvertisingInstance* p_inst = &adv_inst[0];
    for (uint8_t i = 0; i < inst_count; i++, p_inst++) {
      if (p_inst->in_use) continue;

      p_inst->in_use = true;
      p_inst->own_address_type = own_address_type;

      // set up periodic timer to update address.
      if (own_address_type != BLE_ADDR_PUBLIC) {
        if (!rpa_gen_offload_enabled) {
          GenerateRpa(Bind(
            [](AdvertisingInstance* p_inst,
               base::Callback<void(uint8_t, uint8_t)> cb,
               const RawAddress& bda) {

              if (!p_inst->in_use) {
                LOG(ERROR) << "Not active instance";
                return;
              }
              p_inst->own_address = bda;

              alarm_set_on_mloop(p_inst->adv_raddr_timer,
                                 btm_get_next_private_addrress_interval_ms(),
                                 btm_ble_adv_raddr_timer_timeout, p_inst);
              cb.Run(p_inst->inst_id, BTM_BLE_MULTI_ADV_SUCCESS);
            }, p_inst, cb));
        }
        else {
          cb.Run(p_inst->inst_id, BTM_BLE_MULTI_ADV_SUCCESS);
        }
      } else {
        p_inst->own_address = *controller_get_interface()->get_address();

        cb.Run(p_inst->inst_id, BTM_BLE_MULTI_ADV_SUCCESS);
      }
      return;
    }

    LOG(INFO) << "no free advertiser instance";
    cb.Run(0xFF, ADVERTISE_FAILED_TOO_MANY_ADVERTISERS);
  }

  uint8_t GetMaxAdvInstances() {
    return inst_count;
  }

  void UpdateRpaGenOffloadStatus(bool enable) {
    rpa_gen_offload_enabled = enable;
  }

  bool IsRpaGenOffloadEnabled() {
    return rpa_gen_offload_enabled;
  }

  void StartAdvertising(uint8_t advertiser_id, MultiAdvCb cb,
                        tBTM_BLE_ADV_PARAMS* params,
                        std::vector<uint8_t> advertise_data,
                        std::vector<uint8_t> scan_response_data,
                        int duration,
                        MultiAdvCb timeout_cb) override {
    /* a temporary type for holding all the data needed in callbacks below*/
    struct CreatorParams {
      uint8_t inst_id;
      base::WeakPtr<BleAdvertisingManagerImpl> self;
      MultiAdvCb cb;
      tBTM_BLE_ADV_PARAMS params;
      std::vector<uint8_t> advertise_data;
      std::vector<uint8_t> advertise_data_enc;
      std::vector<uint8_t> scan_response_data;
      std::vector<uint8_t> scan_response_data_enc;
      int duration;
      MultiAdvCb timeout_cb;
    };

    std::unique_ptr<CreatorParams> c;
    c.reset(new CreatorParams());

    c->self = weak_factory_.GetWeakPtr();
    c->cb = std::move(cb);
    c->params = *params;
    c->advertise_data = std::move(advertise_data);
    c->scan_response_data = std::move(scan_response_data);
    c->advertise_data_enc;
    c->scan_response_data_enc;
    c->duration = duration;
    c->timeout_cb = std::move(timeout_cb);
    c->inst_id = advertiser_id;

    using c_type = std::unique_ptr<CreatorParams>;

    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->SetParameters(c->inst_id, &c->params, Bind(
      [](c_type c, uint8_t status, int8_t tx_power) {
        if (!c->self) {
          LOG(INFO) << "Stack was shut down";
          return;
        }

        if (status) {
          LOG(ERROR) << "setting parameters failed, status: " << +status;
          c->cb.Run(status);
          return;
        }

        c->self->adv_inst[c->inst_id].tx_power = tx_power;

        const RawAddress& rpa = c->self->adv_inst[c->inst_id].own_address;
        c->self->GetHciInterface()->SetRandomAddress(c->inst_id, rpa, Bind(
          [](c_type c, uint8_t status) {
            if (!c->self) {
              LOG(INFO) << "Stack was shut down";
              return;
            }

            if (status != 0) {
              LOG(ERROR) << "setting random address failed, status: " << +status;
              c->cb.Run(status);
              return;
            }

            c->self->SetData(c->inst_id, false, std::move(c->advertise_data),
                std::move(c->advertise_data_enc),
                    Bind(
                      [](c_type c, uint8_t status) {
                        if (!c->self) {
                          LOG(INFO) << "Stack was shut down";
                          return;
                        }

                        if (status != 0) {
                          LOG(ERROR) << "setting advertise data failed, status: " << +status;
                          c->cb.Run(status);
                          return;
                        }

                        c->self->SetData(c->inst_id, true, std::move(c->scan_response_data),
                            std::move(c->scan_response_data_enc),
                                Bind(
                                  [](c_type c, uint8_t status) {
                                    if (!c->self) {
                                      LOG(INFO) << "Stack was shut down";
                                      return;
                                    }

                                    if (status != 0) {
                                      LOG(ERROR) << "setting scan response data failed, status: "
                                          << +status;
                                      c->cb.Run(status);
                                      return;
                                    }

                                    c->self->Enable(c->inst_id, true, c->cb, c->duration, 0,
                                        std::move(c->timeout_cb));

                                }, base::Passed(&c)));
                    }, base::Passed(&c)));
        }, base::Passed(&c)));
    }, base::Passed(&c)));
    // clang-format on
  }

  void StartAdvertisingSet(IdTxPowerStatusCb cb, tBTM_BLE_ADV_PARAMS* params,
                           std::vector<uint8_t> advertise_data,
                           std::vector<uint8_t> advertise_data_enc,
                           std::vector<uint8_t> scan_response_data,
                           std::vector<uint8_t> scan_response_data_enc,
                           tBLE_PERIODIC_ADV_PARAMS* periodic_params,
                           std::vector<uint8_t> periodic_data,
                           std::vector<uint8_t> periodic_adv_data_enc,
                           uint16_t duration, uint8_t maxExtAdvEvents,
                           std::vector<uint8_t> enc_key_value,
                           RegisterCb timeout_cb) override {
    if (!advertise_data_enc.empty() || !scan_response_data_enc.empty() ||
      !periodic_adv_data_enc.empty()) {
        if (!btm_cb.enc_adv_data_enabled) {
          LOG(ERROR) << __func__ << " Encrypted Advertising Feature" <<
                                    " not Enabled but Encrypted Data is provided";
          cb.Run(0,0,ADVERTISE_FAILED_FEATURE_UNSUPPORTED);
          return;
        }
    }
    std::unique_ptr<CreatorParams> c;
    c.reset(new CreatorParams());

    c->self = weak_factory_.GetWeakPtr();
    c->cb = std::move(cb);
    c->params = *params;
    c->advertise_data = std::move(advertise_data);
    c->advertise_data_enc = std::move(advertise_data_enc);
    c->scan_response_data = std::move(scan_response_data);
    c->scan_response_data_enc = std::move(scan_response_data_enc);
    c->periodic_params = *periodic_params;
    c->periodic_data = std::move(periodic_data);
    c->periodic_adv_data_enc = std::move(periodic_adv_data_enc);
    c->duration = duration;
    c->maxExtAdvEvents = maxExtAdvEvents;
    c->timeout_cb = std::move(timeout_cb);
    c->enc_key_value = std::move(enc_key_value);

    // Check Enc Vectors and Return Error if Encr Adv
    int own_address_type =
        BTM_BleLocalPrivacyEnabled() ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    if (params->own_address_type != BLE_ADDR_ANONYMOUS
        && params->own_address_type != BLE_ADDR_DEFAULT) {
        own_address_type = params->own_address_type;
    }

    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->RegisterAdvertiserImpl(own_address_type, Bind(
      [](c_type c, uint8_t advertiser_id, uint8_t status) {
        if (!c->self) {
          LOG(INFO) << "Stack was shut down";
          return;
        }

        if (status != 0) {
          LOG(ERROR) << " failed, status: " << +status;
          c->cb.Run(0, 0, status);
          return;
        }

        c->inst_id = advertiser_id;
        c->self->adv_inst[c->inst_id].enc_key_value = c->enc_key_value;
        c->self->SetParameters(c->inst_id, &c->params, Bind(
          [](c_type c, uint8_t status, int8_t tx_power) {
            if (!c->self) {
              LOG(INFO) << "Stack was shut down";
              return;
            }

            if (status != 0) {
              c->self->Unregister(c->inst_id);
              LOG(ERROR) << "setting parameters failed, status: " << +status;
              c->cb.Run(0, 0, status);
              return;
            }

            c->self->adv_inst[c->inst_id].tx_power = tx_power;

            if (c->self->adv_inst[c->inst_id].own_address_type == BLE_ADDR_PUBLIC) {
              c->self->StartAdvertisingSetAfterAddressPart(std::move(c));
              return;
            }

            if(!BleAdvertisingManager::Get()->IsRpaGenOffloadEnabled()) {
              //own_address_type == BLE_ADDR_RANDOM
              const RawAddress& rpa = c->self->adv_inst[c->inst_id].own_address;
              c->self->GetHciInterface()->SetRandomAddress(c->inst_id, rpa, Bind(
                [](c_type c, uint8_t status) {
                  if (!c->self) {
                    LOG(INFO) << "Stack was shut down";
                    return;
                  }

                  if (status != 0) {
                    c->self->Unregister(c->inst_id);
                    LOG(ERROR) << "setting random address failed, status: " << +status;
                    c->cb.Run(0, 0, status);
                    return;
                  }
                  c->self->StartAdvertisingSetAfterAddressPart(std::move(c));
              }, base::Passed(&c)));
            }
            else {
              c->self->StartAdvertisingSetAfterAddressPart(std::move(c));
            }
        }, base::Passed(&c)));
    }, base::Passed(&c)));
    // clang-format on
  }

  void StartAdvertisingSetAfterAddressPart(c_type c) {
    c->self->SetData(
        c->inst_id, false, std::move(c->advertise_data), std::move(c->advertise_data_enc),
            Bind(
                [](c_type c, uint8_t status) {
                  if (!c->self) {
                    LOG(INFO) << "Stack was shut down";
                    return;
                  }

                  if (status != 0) {
                    c->self->Unregister(c->inst_id);
                    LOG(ERROR) << "setting advertise data failed, status: "
                              << +status;
                    c->cb.Run(0, 0, status);
                    return;
                  }

                  c->self->SetData(
                      c->inst_id, true, std::move(c->scan_response_data),
                      std::move(c->scan_response_data_enc),
                          Bind(
                              [](c_type c, uint8_t status) {
                                if (!c->self) {
                                  LOG(INFO) << "Stack was shut down";
                                  return;
                                }

                                if (status != 0) {
                                  c->self->Unregister(c->inst_id);
                                  LOG(ERROR)
                                      << "setting scan response data failed, status: "
                                      << +status;
                                  c->cb.Run(0, 0, status);
                                  return;
                                }

                                if (c->periodic_params.enable) {
                                  c->self->StartAdvertisingSetPeriodicPart(
                                      std::move(c));
                                } else {
                                  c->self->StartAdvertisingSetFinish(std::move(c));
                                }
                              },
                              base::Passed(&c)));
                },
                base::Passed(&c)));
  }

  void StartAdvertisingSetPeriodicPart(c_type c) {
    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->SetPeriodicAdvertisingParameters(c->inst_id, &c->periodic_params, Bind(
      [](c_type c, uint8_t status) {
        if (!c->self) {
          LOG(INFO) << "Stack was shut down";
          return;
        }

        if (status != 0) {
          c->self->Unregister(c->inst_id);
          LOG(ERROR) << "setting periodic parameters failed, status: " << +status;
          c->cb.Run(0, 0, status);
          return;
        }

        c->self->SetPeriodicAdvertisingData(c->inst_id, std::move(c->periodic_data),
            std::move(c->periodic_adv_data_enc), Bind(
                [](c_type c, uint8_t status) {
                  if (!c->self) {
                    LOG(INFO) << "Stack was shut down";
                    return;
                  }

                  if (status != 0) {
                    c->self->Unregister(c->inst_id);
                    LOG(ERROR) << "setting periodic parameters failed, status: " << +status;
                    c->cb.Run(0, 0, status);
                    return;
                  }

                  c->self->SetPeriodicAdvertisingEnable(c->inst_id, c->periodic_params.enable, Bind(
                    [](c_type c, uint8_t status) {
                      if (!c->self) {
                        LOG(INFO) << "Stack was shut down";
                        return;
                      }

                      if (status != 0) {
                        c->self->Unregister(c->inst_id);
                        LOG(ERROR) << "enabling periodic advertising failed, status: " << +status;
                        c->cb.Run(0, 0, status);
                        return;
                      }

                      c->self->StartAdvertisingSetFinish(std::move(c));

                    }, base::Passed(&c)));
            }, base::Passed(&c)));
    }, base::Passed(&c)));
    // clang-format on
  }

  void StartAdvertisingSetFinish(c_type c) {
    uint8_t inst_id = c->inst_id;
    uint16_t duration = c->duration;
    uint8_t maxExtAdvEvents = c->maxExtAdvEvents;
    RegisterCb timeout_cb = std::move(c->timeout_cb);
    base::WeakPtr<BleAdvertisingManagerImpl> self = c->self;
    MultiAdvCb enable_cb = Bind(
        [](c_type c, uint8_t status) {
          if (!c->self) {
            LOG(INFO) << "Stack was shut down";
            return;
          }

          if (status != 0) {
            c->self->Unregister(c->inst_id);
            LOG(ERROR) << "enabling advertiser failed, status: " << +status;
            c->cb.Run(0, 0, status);
            return;
          }
          int8_t tx_power = c->self->adv_inst[c->inst_id].tx_power;
          c->cb.Run(c->inst_id, tx_power, status);
        },
        base::Passed(&c));

    self->Enable(inst_id, true, std::move(enable_cb), duration, maxExtAdvEvents,
                 Bind(std::move(timeout_cb), inst_id));
  }

  void EnableWithTimerCb(uint8_t inst_id, MultiAdvCb enable_cb, int duration,
                         MultiAdvCb timeout_cb, uint8_t status) {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    AdvertisingInstance* p_inst = &adv_inst[inst_id];

    // Run the regular enable callback
    enable_cb.Run(status);

    p_inst->timeout_timer = alarm_new("btm_ble.adv_timeout");

    base::Closure cb = Bind(
        &BleAdvertisingManagerImpl::Enable, weak_factory_.GetWeakPtr(), inst_id,
        0 /* disable */, std::move(timeout_cb), 0, 0, base::DoNothing());

    // schedule disable when the timeout passes
    alarm_set_closure(FROM_HERE, p_inst->timeout_timer, duration * 10,
                      std::move(cb));
  }

  void Enable(uint8_t inst_id, bool enable, MultiAdvCb cb, uint16_t duration,
              uint8_t maxExtAdvEvents, MultiAdvCb timeout_cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    VLOG(1) << __func__ << " enable: " << enable << ", duration: " << +duration;
    if (!p_inst->in_use) {
      LOG(ERROR) << "Invalid or no active instance";
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE);
      return;
    }

    if (enable && (duration || maxExtAdvEvents)) {
      p_inst->timeout_cb = std::move(timeout_cb);
    }

    p_inst->duration = duration;
    p_inst->maxExtAdvEvents = maxExtAdvEvents;

    if (!rpa_gen_offload_enabled) {
      if (enable && p_inst->address_update_required) {
        p_inst->address_update_required = false;
        ConfigureRpa(p_inst, base::Bind(&BleAdvertisingManagerImpl::EnableFinish,
                                        weak_factory_.GetWeakPtr(), p_inst,
                                        enable, std::move(cb)));
        return;
      }
    }

    EnableFinish(p_inst, enable, std::move(cb), 0);
  }

  void EnableFinish(AdvertisingInstance* p_inst, bool enable, MultiAdvCb cb,
                    uint8_t status) {
    MultiAdvCb myCb;
    if (enable && p_inst->duration) {
      // TODO(jpawlowski): HCI implementation that can't do duration should
      // emulate it, not EnableWithTimerCb.
      myCb = Bind(&BleAdvertisingManagerImpl::EnableWithTimerCb,
                  weak_factory_.GetWeakPtr(), p_inst->inst_id, std::move(cb),
                  p_inst->duration, p_inst->timeout_cb);
    } else {
      myCb = std::move(cb);

      if (p_inst->timeout_timer) {
        alarm_cancel(p_inst->timeout_timer);
        alarm_free(p_inst->timeout_timer);
        p_inst->timeout_timer = nullptr;
      }
    }

    if (enable) p_inst->enable_time = TimeTicks::Now();
    p_inst->enable_status = enable;
    GetHciInterface()->Enable(enable, p_inst->inst_id, p_inst->duration,
                              p_inst->maxExtAdvEvents, std::move(myCb));
  }

  void SetParameters(uint8_t inst_id, tBTM_BLE_ADV_PARAMS* p_params,
                     ParametersCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    if (!p_inst->in_use) {
      LOG(ERROR) << "adv instance not in use" << +inst_id;
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE, 0);
      return;
    }

    // TODO: disable only if was enabled, currently no use scenario needs
    // that,
    // we always set parameters before enabling
    // GetHciInterface()->Enable(false, inst_id, base::DoNothing());
    p_inst->advertising_event_properties =
        p_params->advertising_event_properties;
    p_inst->tx_power = p_params->tx_power;
    p_inst->advertising_interval = p_params->adv_int_min;
    RawAddress peer_address = RawAddress::kEmpty;

    if (rpa_gen_offload_enabled) {
      //peer addr for RPA offload
      std::string peer_base_addr = "00:00:00:00:00";
      std::string peer_addr_str;
      char buffer[50];
      snprintf (buffer, sizeof(buffer), "%02x", p_inst->inst_id);
      std::string index_str(buffer);
      peer_addr_str = peer_base_addr + ":"+ index_str;
      RawAddress::FromString(peer_addr_str, peer_address);
      p_inst->own_address_type = BLE_ADDR_RANDOM_ID;
    }

    // sid must be in range 0x00 to 0x0F. Since no controller supports more than
    // 16 advertisers, it's safe to make sid equal to inst_id.
    uint8_t sid = p_inst->inst_id % 0x10;

    GetHciInterface()->SetParameters(
        p_inst->inst_id, p_params->advertising_event_properties,
        p_params->adv_int_min, p_params->adv_int_max, p_params->channel_map,
        p_inst->own_address_type, p_inst->own_address, 0x00, peer_address,
        p_params->adv_filter_policy, p_inst->tx_power,
        p_params->primary_advertising_phy, 0x00,
        p_params->secondary_advertising_phy, sid,
        p_params->scan_request_notification_enable, cb);

    // TODO: re-enable only if it was enabled, properly call
    // SetParamsCallback
    // currently no use scenario needs that
    // GetHciInterface()->Enable(true, inst_id, BTM_BleUpdateAdvInstParamCb);
  }

  void SetData(uint8_t inst_id, bool is_scan_rsp, std::vector<uint8_t> data,
               std::vector<uint8_t> encr_data, MultiAdvCb cb) override {
    if (!encr_data.empty() && !btm_cb.enc_adv_data_enabled) {
      LOG(ERROR) << __func__ << " Encrypted Advertising Feature" <<
                                " not Enabled but Encrypted Data is provided";
      cb.Run(ADVERTISE_FAILED_FEATURE_UNSUPPORTED);
      return;
    }
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    bool update_flags = false;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    BleAdvertisingManagerImpl *ptr = instance_weakptr.get();
    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    bool restart = false;
    if (((data.size() + encr_data.size()) > EXT_ADV_DATA_LEN_MAX) && p_inst->IsEnabled()) {
      restart = true;
      GetHciInterface()->Enable(false, inst_id, p_inst->duration,
                                p_inst->maxExtAdvEvents, base::DoNothing());
    }
    if (is_scan_rsp) {
      if (btm_cb.enc_adv_data_log_enabled) {
        VLOG(1) << __func__ << " Scan Response";
      }
      p_inst->scan_response_data = data;
      p_inst->scan_response_data_enc = encr_data;
    } else {
      if (btm_cb.enc_adv_data_log_enabled) {
        VLOG(1) << __func__ << " Advertise";
      }
      p_inst->advertise_data = data;
      p_inst->advertise_data_enc = encr_data;
    }
    if (btm_cb.enc_adv_data_log_enabled) {
      VLOG(1) << __func__ << " Data " << base::HexEncode(data.data(),data.size());
      VLOG(1) << __func__ << " Encr Data " << base::HexEncode(encr_data.data(), encr_data.size());
    }
    if (stack_config_get_interface()->get_pts_le_nonconn_adv_enabled()
       || stack_config_get_interface()->get_pts_le_conn_nondisc_adv_enabled())
      update_flags = true;

    if ((!is_scan_rsp && is_connectable(p_inst->advertising_event_properties))
        || update_flags) {
      uint8_t flags_val = BTM_BLE_NON_DISCOVERABLE;
      if(!stack_config_get_interface()->get_pts_le_conn_nondisc_adv_enabled()) {
        flags_val = BTM_GENERAL_DISCOVERABLE;

        if (p_inst->duration) flags_val = BTM_LIMITED_DISCOVERABLE;
      }

      std::vector<uint8_t> flags;
      flags.push_back(2);  // length
      flags.push_back(HCI_EIR_FLAGS_TYPE);
      flags.push_back(flags_val);

      data.insert(data.begin(), flags.begin(), flags.end());
    }

    /* This is the check to see if there is any data that needs to be encrypted */
    if (!encr_data.empty()) {
      GenerateRandomizer(p_inst,
      Bind(
        [](AdvertisingInstance *p_inst, std::vector<uint8_t> data,
          std::vector<uint8_t> encr_data, BleAdvertisingManagerImpl *ptr,
          bool is_scan_rsp, bool restart, MultiAdvCb cb, uint8_t status) {
          if (status != 0) {
            LOG(ERROR) << " Generating Randomizer Failed" << +status;
            cb.Run(status);
            return;
          }

          for (size_t i = 0; (i + 2) < data.size();) {
            if (data[i + 1] == HCI_EIR_TX_POWER_LEVEL_TYPE) {
              data[i + 2] = p_inst->tx_power;
            }
            i += data[i] + 1;
          }

          for (size_t i = 0; (i + 2) < encr_data.size();) {
            if (encr_data[i + 1] == HCI_EIR_TX_POWER_LEVEL_TYPE) {
              encr_data[i + 2] = p_inst->tx_power;
            }
            i += encr_data[i] + 1;
          }

          encr_data = ptr->EncryptedAdvertising(p_inst, encr_data);
          data.insert(data.end(), encr_data.begin(), encr_data.end());
          if (btm_cb.enc_adv_data_log_enabled) {
            VLOG(1) << __func__ << " Complete Data: " << base::HexEncode(data.data(), data.size());
          }
          if (restart) {
            ptr->DivideAndSendData(p_inst->inst_id, data, false, Bind(
              [](AdvertisingInstance *p_inst, BleAdvertisingManagerImpl *ptr,
                MultiAdvCb cb, uint8_t status) {
                  if (status != 0) {
                    LOG(ERROR) << "Failed to Start Advertisement";
                    cb.Run(status);
                    return;
                  }
                  ptr->GetHciInterface()->Enable(true, p_inst->inst_id, p_inst->duration,
                                            p_inst->maxExtAdvEvents, cb);
              },
              p_inst, ptr, std::move(cb)),
            base::Bind(&BleAdvertisingManagerImpl::SetDataAdvDataSender,
                      ptr->weak_factory_.GetWeakPtr(), is_scan_rsp));
          } else {
            ptr->DivideAndSendData(
                  p_inst->inst_id, data, false, cb,
                  base::Bind(&BleAdvertisingManagerImpl::SetDataAdvDataSender,
                            ptr->weak_factory_.GetWeakPtr(), is_scan_rsp));
          }
        },
        p_inst, data, encr_data, ptr, is_scan_rsp, restart, std::move(cb)));
    } else {
    /* Encr_data is empty so there is no data that needs to be encypted.
      We proceed with Unencrypted Advertising */
    // Find and fill TX Power with the correct value.
    // The TX Power section is a 3 byte section.
      for (size_t i = 0; (i + 2) < data.size();) {
        if (data[i + 1] == HCI_EIR_TX_POWER_LEVEL_TYPE) {
          data[i + 2] = adv_inst[inst_id].tx_power;
        }
        i += data[i] + 1;
      }

      if (restart) {
        DivideAndSendData( p_inst->inst_id, data, false, Bind(
          [](AdvertisingInstance *p_inst, BleAdvertisingManagerImpl *ptr,
            MultiAdvCb cb, uint8_t status) {
            if (status != 0) {
              LOG(ERROR) << "Failed to Start Advertisement";
              cb.Run(status);
              return;
            }
            ptr->GetHciInterface()->Enable(true, p_inst->inst_id, p_inst->duration,
                                      p_inst->maxExtAdvEvents, cb);
          },
          p_inst, ptr, std::move(cb)),
        base::Bind(&BleAdvertisingManagerImpl::SetDataAdvDataSender,
                  weak_factory_.GetWeakPtr(), is_scan_rsp));
      } else {
        DivideAndSendData(
            inst_id, data, false, cb,
            base::Bind(&BleAdvertisingManagerImpl::SetDataAdvDataSender,
                      weak_factory_.GetWeakPtr(), is_scan_rsp));
      }
    }
  }

  void SetDataAdvDataSender(uint8_t is_scan_rsp, uint8_t inst_id,
                            uint8_t operation, uint8_t length, uint8_t* data,
                            MultiAdvCb cb) {
    if (is_scan_rsp)
      GetHciInterface()->SetScanResponseData(inst_id, operation, 0x01, length,
                                             data, cb);
    else
      GetHciInterface()->SetAdvertisingData(inst_id, operation, 0x01, length,
                                            data, cb);
  }

  using DataSender = base::Callback<void(
      uint8_t /*inst_id*/, uint8_t /* operation */, uint8_t /* length */,
      uint8_t* /* data */, MultiAdvCb /* done */)>;

  void DivideAndSendData(int inst_id, std::vector<uint8_t> data, bool is_periodic_adv_data,
                         MultiAdvCb done_cb, DataSender sender) {
    DivideAndSendDataRecursively(true, inst_id, is_periodic_adv_data, std::move(data), 0,
                                 std::move(done_cb), std::move(sender), 0);
  }

  static void DivideAndSendDataRecursively(bool isFirst, int inst_id, bool is_periodic_adv_data,
                                           std::vector<uint8_t> data,
                                           int offset, MultiAdvCb done_cb,
                                           DataSender sender, uint8_t status) {
    constexpr uint8_t INTERMEDIATE =
        0x00;                        // Intermediate fragment of fragmented data
    constexpr uint8_t FIRST = 0x01;  // First fragment of fragmented data
    constexpr uint8_t LAST = 0x02;   // Last fragment of fragmented data
    constexpr uint8_t COMPLETE = 0x03;  // Complete extended advertising data

    int dataSize = (int)data.size();
    if (status != 0 || (!isFirst && offset == dataSize)) {
      /* if we got error writing data, or reached the end of data */
      done_cb.Run(status);
      return;
    }

    uint8_t adv_data_length_max =
        is_periodic_adv_data ? PERIODIC_ADV_DATA_LEN_MAX : EXT_ADV_DATA_LEN_MAX;
    bool moreThanOnePacket = dataSize - offset > adv_data_length_max;
    uint8_t operation = isFirst ? moreThanOnePacket ? FIRST : COMPLETE
                                : moreThanOnePacket ? INTERMEDIATE : LAST;
    int length = moreThanOnePacket ? adv_data_length_max : dataSize - offset;
    int newOffset = offset + length;
    sender.Run(
        inst_id, operation, length, data.data() + offset,
        Bind(&BleAdvertisingManagerImpl::DivideAndSendDataRecursively, false,
             inst_id, is_periodic_adv_data, std::move(data), newOffset, std::move(done_cb),
             sender));
  }

  void SetPeriodicAdvertisingParameters(uint8_t inst_id,
                                        tBLE_PERIODIC_ADV_PARAMS* params,
                                        MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;

    GetHciInterface()->SetPeriodicAdvertisingParameters(
        inst_id, params->min_interval, params->max_interval,
        params->periodic_advertising_properties, cb);
  }

  void SetPeriodicAdvertisingData(uint8_t inst_id, std::vector<uint8_t> data,
                                  std::vector<uint8_t> encr_data,
                                  MultiAdvCb cb) override {
    if (!encr_data.empty() && !btm_cb.enc_adv_data_enabled) {
      LOG(ERROR) << __func__ << " Encrypted Advertising Feature" <<
                                " not Enabled but Encrypted Data is provided";
      cb.Run(ADVERTISE_FAILED_FEATURE_UNSUPPORTED);
      return;
    }
    VLOG(1) << __func__ << " inst_id: " << +inst_id;

    BleAdvertisingManagerImpl *ptr = instance_weakptr.get();
    if (ptr == nullptr) return;
    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    p_inst->periodic_data = data;
    p_inst->periodic_adv_data_enc = encr_data;

    bool restartPeriodic = false;
    bool restart = false;
    if (((data.size() + encr_data.size()) > PERIODIC_ADV_DATA_LEN_MAX)) {
      if (p_inst->periodic_enabled) {
        SetPeriodicAdvertisingEnable(inst_id, false, base::DoNothing());
        restartPeriodic = true;
      }
    }

    if (btm_cb.enc_adv_data_log_enabled) {
      VLOG(1) << __func__ << " Data: " << base::HexEncode(data.data(), data.size());
      VLOG(1) << __func__ << " Encr Data: " << base::HexEncode(encr_data.data(), encr_data.size());
    }

    if ((data.size() > 3) && (data[0] == 3 && data[1] == 0x16
         && data[2] == 0x51 && data[3] == 0x18)) {
      VLOG(1) << __func__ << "Broadcast UUID";
      adv_inst[inst_id].skip_rpa_count = 15;
      adv_inst[inst_id].skip_rpa = true;
    }

    if ((encr_data.size() > 3) && (encr_data[0] == 3 && encr_data[1] == 0x16
         && encr_data[2] == 0x51 && encr_data[3] == 0x18)) {
      VLOG(1) << __func__ << "Broadcast UUID";
      adv_inst[inst_id].skip_rpa_count = 15;
      adv_inst[inst_id].skip_rpa = true;
    }

    /* This is the check to see if there is any periodic advertising data that needs to be encrypted */
    if (!encr_data.empty()) {
      GenerateRandomizer(p_inst,
      Bind(
        [](AdvertisingInstance *p_inst, std::vector<uint8_t> data, std::vector<uint8_t> encr_data,
        BleAdvertisingManagerImpl *ptr, bool restart, bool restartPeriodic,
        MultiAdvCb cb, uint8_t status){
          if (status != 0) {
            LOG(ERROR) << " Generating Randomizer Failed" << +status;
            cb.Run(status);
            return;
          }
          encr_data = ptr->EncryptedAdvertising(p_inst, encr_data);
          data.insert(data.end(), encr_data.begin(), encr_data.end());
          if (btm_cb.enc_adv_data_log_enabled) {
            VLOG(1) << __func__ << " Complete Data: " << base::HexEncode(data.data(), data.size());
          }
          if (restartPeriodic) {
            ptr->DivideAndSendData(p_inst->inst_id, data, true, Bind(
              [](AdvertisingInstance *p_inst, BleAdvertisingManagerImpl *ptr,
                bool restart, MultiAdvCb cb, uint8_t status) {
                  if (status != 0) {
                    LOG(ERROR) << "Failed to Start Advertisement";
                    cb.Run(status);
                    return;
                  }
                  ptr->SetPeriodicAdvertisingEnable(p_inst->inst_id,true, cb);
              },
              p_inst, ptr, restart, std::move(cb)),
            base::Bind(&BleAdvertiserHciInterface::SetPeriodicAdvertisingData,
                base::Unretained(ptr->GetHciInterface())));
          } else {
          ptr->DivideAndSendData(
              p_inst->inst_id, data, true, cb,
              base::Bind(&BleAdvertiserHciInterface::SetPeriodicAdvertisingData,
                        base::Unretained(ptr->GetHciInterface())));
          }
        },
        p_inst, data, encr_data, ptr, restart, restartPeriodic, std::move(cb)));
    } else {
      /* Proceed with unencrypted periodic advertising */
      if (restartPeriodic) {
        ptr->DivideAndSendData(
          p_inst->inst_id, data, true, Bind(
            [](AdvertisingInstance *p_inst, BleAdvertisingManagerImpl *ptr,
            MultiAdvCb cb, uint8_t status) {
              if (status != 0) {
                LOG(ERROR) << "Failed to Start Advertisement";
                cb.Run(status);
                return;
              }
              ptr->SetPeriodicAdvertisingEnable(p_inst->inst_id,true, cb);
            },
            p_inst, ptr, std::move(cb)),
          base::Bind(&BleAdvertiserHciInterface::SetPeriodicAdvertisingData,
              base::Unretained(ptr->GetHciInterface())));
      } else {
      DivideAndSendData(
          inst_id, data, true, cb,
          base::Bind(&BleAdvertiserHciInterface::SetPeriodicAdvertisingData,
                    base::Unretained(GetHciInterface())));
      }
    }
  }

  void SetPeriodicAdvertisingEnable(uint8_t inst_id, uint8_t enable,
                                    MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id << ", enable: " << +enable;

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    if (!p_inst->in_use) {
      LOG(ERROR) << "Invalid or not active instance";
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE);
      return;
    }

    MultiAdvCb enable_cb = Bind(
        [](AdvertisingInstance* p_inst, uint8_t enable, MultiAdvCb cb,
           uint8_t status) {
          VLOG(1) << "periodc adv enable cb: inst_id: " << +p_inst->inst_id
                  << ", enable: " << +enable << ", status: " << std::hex
                  << +status;
          if (!status) p_inst->periodic_enabled = enable;

          cb.Run(status);
        },
        p_inst, enable, std::move(cb));

    if (enable != 0) {
      if (!controller_get_interface()->supports_ble_periodic_advertising_adi()) {
        enable = 1; // use value of 0x01 if ADI is not supported
      }
    }
    GetHciInterface()->SetPeriodicAdvertisingEnable(enable, inst_id,
                                                    std::move(enable_cb));
  }

  void CreateBIG(uint8_t inst_id, tBLE_CREATE_BIG_PARAMS* params,
                 CreateBIGCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;

    if (!controller_get_interface()->supports_ble_iso_broadcaster()) {
      VLOG(1) << __func__ << " Iso Broadcaster not supported in controller:";
      if (cb) {
        std::vector<uint16_t> conn_hdl_list;
        cb.Run(inst_id, HCI_ERR_ILLEGAL_COMMAND, INVALID_BIG_HANDLE, 0,
               0, 0, 0, 0, 0, 0, 0, 0, 0, conn_hdl_list);
      }
      return;
    }

    uint8_t i=0;
    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    IsoBIGInstance* p_big_inst = &iso_big_inst[0];

    for (i = 0; i < inst_count; i++, p_big_inst++) {
      if (p_big_inst->in_use) continue;

      p_big_inst->in_use = true;
      p_big_inst->big_handle = i;
      p_big_inst->adv_inst_id = inst_id;
      p_big_inst->create_big_cb = cb;
      VLOG(1) << __func__ << "BIG handle allocated:" << +i;
      break;
    }
    if (i == inst_count) {
      VLOG(1) << __func__ << "Cant Create BIG, Max BIG Handle limit reached:"
              << +inst_count;
      if (cb) {
        std::vector<uint16_t> conn_hdl_list;
        cb.Run(inst_id, HCI_ERR_ILLEGAL_COMMAND, INVALID_BIG_HANDLE, 0,
               0, 0, 0, 0, 0, 0, 0, 0, 0, conn_hdl_list);
      }
      return;
    }

    p_inst->big_handle = p_big_inst->big_handle;

    GetHciInterface()->CreateBIG(p_inst->big_handle,
          inst_id, params->num_bis, params->sdu_int, params->max_sdu,
          params->max_transport_latency, params->rtn, params->phy,
          params->packing, params->framing, params->encryption,
          params->broadcast_code);
  }

  void TerminateBIG(uint8_t inst_id, uint8_t big_handle,
                    uint8_t reason, TerminateBIGCb cb) override {
    VLOG(1) << __func__ << " big_handle: " << +big_handle;

    if (!controller_get_interface()->supports_ble_iso_broadcaster()) {
      VLOG(1) << __func__ << " Iso Broadcaster not supported in controller:";
      if (cb) {
        cb.Run(HCI_ERR_ILLEGAL_COMMAND, inst_id, big_handle, reason);
      }
      return;
    }

    if (big_handle >= inst_count) {
      LOG(ERROR) << " Invalid BIG handle";
      if (cb) {
        cb.Run(HCI_ERR_ILLEGAL_COMMAND, inst_id, big_handle, reason);
      }
      return;
    }
    IsoBIGInstance* p_big_inst = &iso_big_inst[big_handle];

    std::lock_guard<std::mutex> lock(lock_);
    if (!BleAdvertisingManager::IsInitialized()) {
      LOG(ERROR) << "Stack already shutdown";
      if (cb) {
        cb.Run(HCI_ERR_ILLEGAL_COMMAND, inst_id, big_handle, reason);
      }
      return;
    }

    p_big_inst->terminate_big_cb = cb;
    p_big_inst->adv_inst_id = inst_id;

    if (p_big_inst->IsCreated()) {
      GetHciInterface()->TerminateBIG(big_handle, reason);
    }
    else {
      LOG(ERROR) << "Terminating BIG which is not created";
      if (cb) {
        cb.Run(HCI_ERR_ILLEGAL_COMMAND, inst_id, big_handle, reason);
      }
    }
  }

  void Unregister(uint8_t inst_id) override {
    AdvertisingInstance* p_inst = &adv_inst[inst_id];

    VLOG(1) << __func__ << " inst_id: " << +inst_id;

    std::lock_guard<std::mutex> lock(lock_);
    if (!BleAdvertisingManager::IsInitialized()) {
      LOG(ERROR) << "Stack already shutdown";
      return;
    }

    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    if (controller_get_interface()->supports_ble_iso_broadcaster()) {
      //Terminate BIG
      if (p_inst->big_handle != INVALID_BIG_HANDLE) {
        IsoBIGInstance* p_big_inst = &iso_big_inst[p_inst->big_handle];
        GetHciInterface()->TerminateBIG(p_inst->big_handle,
                                        HCI_ERR_CONN_CAUSE_LOCAL_HOST);

        p_big_inst->in_use = false;
        p_big_inst->bis_handles.clear();
        p_big_inst->created_status = false;
        p_big_inst->big_handle = INVALID_BIG_HANDLE;
        p_inst->big_handle = INVALID_BIG_HANDLE;
      }
    }

    if (adv_inst[inst_id].IsEnabled()) {
      p_inst->enable_status = false;
      p_inst->advertise_data.clear();
      p_inst->advertise_data_enc.clear();
      p_inst->scan_response_data.clear();
      p_inst->scan_response_data_enc.clear();
      GetHciInterface()->Enable(false, inst_id, 0x00, 0x00, base::DoNothing());
    }

    if (p_inst->periodic_enabled) {
      p_inst->periodic_enabled = false;
      p_inst->periodic_data.clear();
      p_inst->periodic_adv_data_enc.clear();
      GetHciInterface()->SetPeriodicAdvertisingEnable(false, inst_id,
                                                      base::DoNothing());
    }

    if (p_inst->timeout_timer) {
      VLOG(1) << __func__ << " Cancelling timer for inst_id: " << +inst_id;
      alarm_cancel(p_inst->timeout_timer);
      alarm_free(p_inst->timeout_timer);
      p_inst->timeout_timer = nullptr;
    }

    alarm_cancel(p_inst->adv_raddr_timer);
    p_inst->in_use = false;
    p_inst->skip_rpa_count = 0;
    p_inst->skip_rpa = false;
    GetHciInterface()->RemoveAdvertisingSet(inst_id, base::DoNothing());
    p_inst->address_update_required = false;
  }

  void RecomputeTimeout(AdvertisingInstance* inst, TimeTicks now) {
    TimeDelta duration = now - inst->enable_time;
    bool cb_fired = false;
    if (inst->duration) {
      int durationDone = (duration.InMilliseconds() / 10);
      if (durationDone + 1 >= inst->duration) {
        inst->enable_status = false;
        inst->timeout_cb.Run(0 /* TODO: STATUS HERE?*/);
        cb_fired = true;
      } else {
        inst->duration = inst->duration - durationDone;
      }
    }

    if (inst->maxExtAdvEvents && !cb_fired) {
      int eventsDone =
          (duration.InMilliseconds() / (inst->advertising_interval * 5 / 8));

      if (eventsDone + 1 >= inst->maxExtAdvEvents) {
        inst->enable_status = false;
        inst->timeout_cb.Run(0 /* TODO: STATUS HERE?*/);
      } else {
        inst->maxExtAdvEvents = inst->maxExtAdvEvents - eventsDone;
      }
    }
  }

  void Suspend() {
    std::vector<SetEnableData> sets;

    for (AdvertisingInstance& inst : adv_inst) {
      if (!inst.in_use || !inst.enable_status) continue;

      if (inst.duration || inst.maxExtAdvEvents)
        RecomputeTimeout(&inst, TimeTicks::Now());

      sets.emplace_back(SetEnableData{.handle = inst.inst_id});
    }

    if (!sets.empty())
      GetHciInterface()->Enable(false, sets, base::DoNothing());
  }

  void Resume() override {
    std::vector<SetEnableData> sets;

    for (const AdvertisingInstance& inst : adv_inst) {
      if (inst.in_use && inst.enable_status) {
        sets.emplace_back(SetEnableData{
            .handle = inst.inst_id,
            .duration = inst.duration,
            .max_extended_advertising_events = inst.maxExtAdvEvents});
      }
    }

    if (!sets.empty()) GetHciInterface()->Enable(true, sets, base::DoNothing());
  }

  void OnAdvertisingSetTerminated(
      uint8_t status, uint8_t advertising_handle, uint16_t connection_handle,
      uint8_t num_completed_extended_adv_events) override {
    AdvertisingInstance* p_inst = &adv_inst[advertising_handle];
    VLOG(1) << __func__ << "status: " << loghex(status)
            << ", advertising_handle: " << loghex(advertising_handle)
            << ", connection_handle: " << loghex(connection_handle);

    if (status == HCI_ERR_LIMIT_REACHED ||
        status == HCI_ERR_ADVERTISING_TIMEOUT) {
      // either duration elapsed, or maxExtAdvEvents reached
      p_inst->enable_status = false;

      if (p_inst->timeout_cb.is_null()) {
        LOG(INFO) << __func__ << "No timeout callback";
        return;
      }

      p_inst->timeout_cb.Run(status);
      return;
    }

    if (!rpa_gen_offload_enabled) {
      if (BTM_BleLocalPrivacyEnabled() &&
          advertising_handle <= BTM_BLE_MULTI_ADV_MAX) {
        btm_acl_update_conn_addr(connection_handle, p_inst->own_address);
      }
    }

    VLOG(1) << "reneabling advertising";

    if (p_inst->in_use) {
      // TODO(jpawlowski): we don't really allow to do directed advertising
      // right now. This should probably be removed, check with Andre.
      if ((p_inst->advertising_event_properties & 0x0C) == 0) {
        /* directed advertising bits not set */

        RecomputeTimeout(p_inst, TimeTicks::Now());
        if (p_inst->enable_status) {
          GetHciInterface()->Enable(true, advertising_handle, p_inst->duration,
                                    p_inst->maxExtAdvEvents, base::DoNothing());
        }
      } else {
        /* mark directed adv as disabled if adv has been stopped */
        p_inst->in_use = false;
      }
    }
  }

  void CreateBIGComplete(
      uint8_t status, uint8_t big_handle, uint32_t big_sync_delay,
      uint32_t transport_latency_big, uint8_t phy, uint8_t nse,
      uint8_t bn, uint8_t pto, uint8_t irc, uint16_t max_pdu,
      uint16_t iso_int, uint8_t num_bis,
      std::vector<uint16_t> conn_handle_list) override {
    VLOG(1) << __func__ << " big_handle: " << +big_handle << "status:" << +status;

    if (big_handle >= inst_count) {
      LOG(ERROR) << " Invalid BIG handle";
      return;
    }
    IsoBIGInstance* p_big_inst = &iso_big_inst[big_handle];

    if (status == HCI_SUCCESS) {
      p_big_inst->bis_handles = conn_handle_list;
      p_big_inst->created_status = true;
    }
    else {
      p_big_inst->in_use = false;
      p_big_inst->big_handle = INVALID_BIG_HANDLE;

      AdvertisingInstance* p_inst = &adv_inst[p_big_inst->adv_inst_id];
      p_inst->big_handle = INVALID_BIG_HANDLE;
    }

    if (p_big_inst->create_big_cb) {
      p_big_inst->create_big_cb.Run(p_big_inst->adv_inst_id, status, big_handle,
                                    big_sync_delay, transport_latency_big, phy,
                                    nse, bn, pto, irc, max_pdu, iso_int, num_bis,
                                    conn_handle_list);
    }
  }

  void TerminateBIGComplete(uint8_t status, uint8_t big_handle,
                            bool cmd_status, uint8_t reason) override {
    VLOG(1) << __func__ << " big_handle: " << +big_handle;

    if (big_handle >= inst_count) {
      LOG(ERROR) << " Invalid BIG handle";
      return;
    }
    IsoBIGInstance* p_big_inst = &iso_big_inst[big_handle];

    if (!cmd_status) {
      p_big_inst->in_use = false;
      p_big_inst->bis_handles.clear();
      p_big_inst->created_status = false;
      p_big_inst->big_handle = INVALID_BIG_HANDLE;

      AdvertisingInstance* p_inst = &adv_inst[p_big_inst->adv_inst_id];
      p_inst->big_handle = INVALID_BIG_HANDLE;
    }

    if (p_big_inst->terminate_big_cb) {
      p_big_inst->terminate_big_cb.Run(status, p_big_inst->adv_inst_id, big_handle, reason);
    }
  }

  base::WeakPtr<BleAdvertisingManagerImpl> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void CancelAdvAlarms() {
    AdvertisingInstance* p_inst = &adv_inst[0];
    for (uint8_t i = 0; i < inst_count; i++, p_inst++) {
      if (p_inst->timeout_timer) {
        alarm_cancel(p_inst->timeout_timer);
      }
      if (p_inst->adv_raddr_timer) {
        alarm_cancel(p_inst->adv_raddr_timer);
      }
    }
  }

 private:
  BleAdvertiserHciInterface* GetHciInterface() { return hci_interface; }

  BleAdvertiserHciInterface* hci_interface = nullptr;
  std::vector<AdvertisingInstance> adv_inst;
  uint8_t inst_count;
  bool rpa_gen_offload_enabled;
  std::vector<IsoBIGInstance> iso_big_inst;

  // Member variables should appear before the WeakPtrFactory, to ensure
  // that any WeakPtrs are invalidated before its members
  // variable's destructors are executed, rendering them invalid.
  base::WeakPtrFactory<BleAdvertisingManagerImpl> weak_factory_;
};

void btm_ble_adv_raddr_timer_timeout(void* data) {
  BleAdvertisingManagerImpl* ptr = instance_weakptr.get();
  if (ptr) ptr->ConfigureRpa((AdvertisingInstance*)data, base::DoNothing());
}
}  // namespace

void BleAdvertisingManager::Initialize(BleAdvertiserHciInterface* interface) {
  instance = new BleAdvertisingManagerImpl(interface);
  instance_weakptr = ((BleAdvertisingManagerImpl*)instance)->GetWeakPtr();
}

bool BleAdvertisingManager::IsInitialized() { return instance; }

base::WeakPtr<BleAdvertisingManager> BleAdvertisingManager::Get() {
  return instance_weakptr;
};

void BleAdvertisingManager::CleanUp() {
  if (instance_weakptr.get()) instance_weakptr.get()->CancelAdvAlarms();

  delete instance;
  instance = nullptr;
};

/**
 * This function initialize the advertising manager.
 **/
void btm_ble_adv_init() {
  BleAdvertiserHciInterface::Initialize();
  BleAdvertisingManager::Initialize(BleAdvertiserHciInterface::Get());
  BleAdvertiserHciInterface::Get()->SetAdvertisingEventObserver(
      (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get().get());

  if (BleAdvertiserHciInterface::Get()->QuirkAdvertiserZeroHandle()) {
    // If handle 0 can't be used, register advertiser for it, but never use it.
    BleAdvertisingManager::Get().get()->RegisterAdvertiser(base::DoNothing());
  }
  auto ble_adv_mgr_ptr = (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get().get();
  if (ble_adv_mgr_ptr) {
    ble_adv_mgr_ptr->UpdateRpaGenOffloadStatus(btm_cb.rpa_gen_offload_enabled);
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_multi_adv_cleanup
 *
 * Description      This function cleans up multi adv control block.
 *
 * Parameters
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_multi_adv_cleanup(void) {
  std::lock_guard<std::mutex> lock(lock_);
  BleAdvertisingManager::CleanUp();
  BleAdvertiserHciInterface::CleanUp();
}

uint8_t btm_ble_get_max_adv_instances(void) {
  auto ble_adv_mgr_ptr = (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get().get();
  return (ble_adv_mgr_ptr ? ble_adv_mgr_ptr->GetMaxAdvInstances() : 0);
}

// TODO(jpawlowski): Find a nicer way to test RecomputeTimeout without exposing
// AdvertisingInstance
bool timeout_triggered = false;
void test_timeout_cb(uint8_t status) { timeout_triggered = true; }

// verify that if duration passed, or is about to pass, recomputation will shut
// down the advertiser completly
void testRecomputeTimeout1() {
  auto manager = (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get().get();

  TimeTicks start = TimeTicks::Now();
  TimeTicks end = start + TimeDelta::FromMilliseconds(111);
  AdvertisingInstance test1(0);
  test1.enable_status = true;
  test1.enable_time = start;
  test1.duration = 12 /*120ms*/;
  test1.timeout_cb = Bind(&test_timeout_cb);

  manager->RecomputeTimeout(&test1, end);

  CHECK(timeout_triggered);
  timeout_triggered = false;
  CHECK(!test1.enable_status);
}

// verify that duration and maxExtAdvEvents are properly adjusted when
// recomputing.
void testRecomputeTimeout2() {
  auto manager = (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get().get();

  TimeTicks start = TimeTicks::Now();
  TimeTicks end = start + TimeDelta::FromMilliseconds(250);
  AdvertisingInstance test1(0);
  test1.enable_status = true;
  test1.enable_time = start;
  test1.duration = 50 /*500ms*/;
  test1.maxExtAdvEvents = 50;
  test1.advertising_interval = 16 /* 10 ms */;
  test1.timeout_cb = Bind(&test_timeout_cb);

  manager->RecomputeTimeout(&test1, end);

  CHECK(!timeout_triggered);
  CHECK(test1.enable_status);
  CHECK(test1.duration == 25);
  CHECK(test1.maxExtAdvEvents == 25);
}

// verify that if maxExtAdvEvents were sent, or are close to end, recomputation
// wil shut down the advertiser completly
void testRecomputeTimeout3() {
  auto manager = (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get().get();

  TimeTicks start = TimeTicks::Now();
  TimeTicks end = start + TimeDelta::FromMilliseconds(495);
  AdvertisingInstance test1(0);
  test1.enable_status = true;
  test1.enable_time = start;
  test1.maxExtAdvEvents = 50;
  test1.advertising_interval = 16 /* 10 ms */;
  test1.timeout_cb = Bind(&test_timeout_cb);

  manager->RecomputeTimeout(&test1, end);

  CHECK(timeout_triggered);
  timeout_triggered = false;
  CHECK(!test1.enable_status);
}
