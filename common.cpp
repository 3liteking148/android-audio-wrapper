/*
 * Copyright (C) 2013 Thomas Wendt <thoemy@gmx.net>
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

#define LOG_TAG "AudioWrapperCommon"
// #define LOG_NDEBUG 0

#include <string.h>
#include <limits.h>

#include <cutils/log.h>

#include "common.h"

int load_vendor_module(const hw_module_t* wrapper_module, const char* name,
                       hw_device_t** device, const char* inst)
{
    const hw_module_t* module;
    char module_name[PATH_MAX];
    int ret = 0;
    ALOGI("%s", __FUNCTION__);

    // Create the module name based on the wrapper module id and inst.
    if(inst)
        snprintf(module_name, PATH_MAX, "vendor-%s.%s", wrapper_module->id, inst);
    else
        snprintf(module_name, PATH_MAX, "vendor-%s", wrapper_module->id);

    ret = hw_get_module(module_name, &module);
    ALOGE_IF(ret, "%s: couldn't load vendor module %s (%s)", __FUNCTION__,
             module_name, strerror(-ret));
    if (ret)
        goto out;

    ret = module->methods->open(module, name, device);
    ALOGE_IF(ret, "%s: couldn't open hw device in %s (%s)", __FUNCTION__,
             module_name, strerror(-ret));
    if(ret)
        goto out;

    return 0;
 out:
    *device = NULL;
    return ret;
}

#ifdef CONVERT_AUDIO_DEVICES_T
static audio_devices_t convert_ics_to_jb(const ics_audio_devices_t ics_device)
{
    audio_devices_t device = 0;

    if((ics_device & ~ICS_AUDIO_DEVICE_OUT_ALL) == 0) {
        // The first 15 AUDIO_DEVICE_OUT bits are equal. Exception is
        // ICS_AUDIO_DEVICE_OUT_DEFAULT / AUDIO_DEVICE_OUT_REMOTE_SUBMIX.
        device = ics_device & ~ICS_AUDIO_DEVICE_OUT_DEFAULT;

        // Set the correct DEFAULT bit
        if(ics_device & ICS_AUDIO_DEVICE_OUT_DEFAULT) {
            device |= AUDIO_DEVICE_OUT_DEFAULT;
        }
    } else if((ics_device & ~ICS_AUDIO_DEVICE_IN_ALL) == 0) {
        // Bits needs to be shifted 16 bits to the right and the IN bit must be
        // set.
        device = ((ics_device & ~ICS_AUDIO_DEVICE_IN_DEFAULT) >> 16) | AUDIO_DEVICE_BIT_IN;

        // Set the correct DEFAULT bit
        if((ics_device & ICS_AUDIO_DEVICE_IN_DEFAULT) == ICS_AUDIO_DEVICE_IN_DEFAULT) {
            device |= AUDIO_DEVICE_IN_DEFAULT;
        }
    } else {
        // ics_device has bits for input and output devices set and cannot be
        // properly converted to a JB 4.2 representation.
        ALOGW("%s: 0x%x has no proper representation", __FUNCTION__, ics_device);
        device = ics_device;
    }

    return device;
}

#define DEVICE_OUT_MASK 0x3FFF
#define DEVICE_IN_MASK 0xFF

static ics_audio_devices_t convert_jb_to_ics(const audio_devices_t device)
{
    ics_audio_devices_t ics_device = 0;

    if(audio_is_output_devices(device)) {
        // We care only about the first 15 bits since the others cannot be
        // mapped to the old enum.
        ics_device = (device & DEVICE_OUT_MASK);
        // Set the correct DEFAULT bit
        if(device & AUDIO_DEVICE_OUT_DEFAULT)
            ics_device |= ICS_AUDIO_DEVICE_OUT_DEFAULT;
    } else if((device & AUDIO_DEVICE_BIT_IN) == AUDIO_DEVICE_BIT_IN) {
        // We care only about the first 8 bits since the other cannot be
        // mapped to the old enum.
        ics_device = (device & DEVICE_IN_MASK) << 16;
        if((device & AUDIO_DEVICE_IN_DEFAULT) == AUDIO_DEVICE_IN_DEFAULT)
            ics_device |= ICS_AUDIO_DEVICE_IN_DEFAULT;
    } else {
        // I guess we should never land here
        ALOGW("%s: audio_devices_t is neither input nor output: 0x%x",
              __FUNCTION__, device);
        ics_device = device;
    }

    return ics_device;
}
#endif

static ics_audio_devices_t fixup_audio_devices(ics_audio_devices_t device)
{
#ifdef NO_HTC_POLICY_MANAGER
    // audio_policy.default wants to open BUILTIN_MIC for some input source
    // which results in silence. The HTC audio_policy uses VOICE_CALL
    // instead. Also the BUILTIN_MIC bit of get_supported_devices() is not
    // set. So this seems the correct thing to do.
    if((device & ICS_AUDIO_DEVICE_IN_BUILTIN_MIC) == ICS_AUDIO_DEVICE_IN_BUILTIN_MIC) {
        ALOGI("%s: BUILTIN_MIC set, setting VOICE_CALL instead", __FUNCTION__);
        device &= ~ICS_AUDIO_DEVICE_IN_BUILTIN_MIC;
        device |= ICS_AUDIO_DEVICE_IN_VOICE_CALL;
    }
#endif
    return device;
}

uint32_t convert_audio_devices(const uint32_t devices, flags_conversion_mode_t mode)
{
    uint32_t ret;
    switch(mode) {
    case ICS_TO_JB:
#ifdef CONVERT_AUDIO_DEVICES_T
        ret = convert_ics_to_jb(devices);
#else
        ret = devices;
#endif
        ALOGI("%s: ICS_TO_JB (0x%x -> 0x%x)", __FUNCTION__, devices, ret);
        break;
    case JB_TO_ICS:
#ifdef CONVERT_AUDIO_DEVICES_T
        ret = convert_jb_to_ics(devices);
#else
        ret = devices;
#endif
        ret = fixup_audio_devices(ret);
        ALOGI("%s: JB_TO_ICS (0x%x -> 0x%x)", __FUNCTION__, devices, ret);
        break;
    default:
        ALOGE("%s: Invalid conversion mode %d", __FUNCTION__, mode);
        ret = devices;
    }

    return ret;
}

char * fixup_audio_parameters(const char *kv_pairs, flags_conversion_mode_t mode)
{
    int value;
    size_t len;
    char *out;

    android::AudioParameter param = android::AudioParameter(android::String8(kv_pairs));
    android::String8 key = android::String8(android::AudioParameter::keyRouting);

    // TODO: There are more parameters that pass audio_devices_t values
    if (param.getInt(key, value) == android::NO_ERROR) {
        ALOGI("%s: Fixing routing value (value: %x, mode: %d)", __FUNCTION__,
              value, mode);
        value = convert_audio_devices(value, mode);

        // Adds value as a singed int that might be negative. Doesn't cause any
        // problems because the bit representation is the same.
        param.addInt(key, value);

        // When param is freed the string returned by param.toString() seems to
        // be freed as well, so copy it.
        android::String8 fixed_kv_pairs = param.toString();
        len = fixed_kv_pairs.length();
        out = (char *) malloc(len + 1);
        strcpy(out, fixed_kv_pairs.string());

        ALOGI("%s: fixed_kv_pairs: %s (%d)", __FUNCTION__, out, strlen(out));
    } else {
        len = strlen(kv_pairs);
        out = (char *) malloc(len + 1);
        strcpy(out, kv_pairs);
    }

    return out;
}
