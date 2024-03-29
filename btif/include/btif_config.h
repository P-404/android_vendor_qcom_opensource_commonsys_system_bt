/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
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
 ******************************************************************************/

/*******************************************************************************
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 *******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "bt_types.h"

#include <list>
#include <string>
#include "osi/include/config.h"

#define A2DP_VERSION_CONFIG_KEY "A2dpVersion"
#define AVDTP_VERSION_CONFIG_KEY "AvdtpVersion"
#define HFP_VERSION_CONFIG_KEY "HfpVersion"
#define AV_REM_CTRL_VERSION_CONFIG_KEY "AvrcpCtVersion"
#define AV_REM_CTRL_TG_VERSION_CONFIG_KEY "AvrcpTgVersion"
#define AV_REM_CTRL_FEATURES_CONFIG_KEY "AvrcpFeatures"
#define PBAP_PCE_VERSION_CONFIG_KEY "PbapPceVersion"
#define MAP_MCE_VERSION_CONFIG_KEY "MapMceVersion"
#define PNP_VENDOR_ID_CONFIG_KEY "VendorID"
#define PNP_PRODUCT_ID_CONFIG_KEY "ProductID"
#define PNP_PRODUCT_VERSION_CONFIG_KEY "ProductVersion"

static const char BTIF_CONFIG_MODULE[] = "btif_config_module";

typedef struct btif_config_section_iter_t btif_config_section_iter_t;

bool btif_config_has_section(const std::string& section);
bool btif_config_has_section(const char* section);
bool btif_config_exist(const std::string& section, const std::string& key);
bool btif_config_get_int(const std::string& section, const std::string& key, int* value);
bool btif_config_set_int(const std::string& section, const std::string& key, int value);
bool btif_config_get_uint16(const char* section, const char* key, uint16_t* value);
bool btif_config_set_uint16(const std::string& section, const std::string& key, uint16_t value);
bool btif_config_get_uint64(const char* section, const char* key, uint64_t* value);
bool btif_config_set_uint64(const std::string& section, const std::string& key,
                            uint64_t value);
bool btif_config_get_str(const std::string& section, const std::string& key, char* value,
                         int* size_bytes);
bool btif_config_set_str(const std::string& section, const std::string& key,
                         const char* value);
bool btif_config_set_str(const char* section, const char* key,const char* value);
bool btif_config_get_bin(const std::string& section, const std::string& key, uint8_t* value,
                         size_t* length);
bool btif_config_get_key_from_bin(const char* section, const char* key);
bool btif_config_get_key_from_bin(const std::string& section, const std::string& key);
bool btif_config_set_uint64(const char* section, const char* key,
                            uint64_t value);
bool btif_config_set_bin(const std::string& section, const std::string& key,
                         const uint8_t* value, size_t length);
bool btif_config_remove(const std::string& section, const std::string& key);

size_t btif_config_get_bin_length(const std::string& section, const std::string& key);

std::vector<RawAddress> btif_config_get_paired_devices();

void btif_config_save(void);
void btif_config_flush(void);
bool btif_config_clear(void);

// TODO(zachoverflow): Eww...we need to move these out. These are peer specific,
// not config general.
bool btif_get_address_type(const RawAddress& bd_addr, int* p_addr_type);
bool btif_get_device_type(const RawAddress& bd_addr, int* p_device_type);

void btif_debug_config_dump(int fd);
