/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef ANDROID_SENSORS_WRAPPER_H
#define ANDROID_SENSORS_WRAPPER_H

#include "android/hardware/sensors/1.0/ISensors.h"

#include <utils/LightRefBase.h>

namespace android {
namespace SensorServiceUtil {

using ::android::hardware::Return;
using ::android::hardware::sensors::V1_0::Event;
using ::android::hardware::sensors::V1_0::ISensors;
using ::android::hardware::sensors::V1_0::Result;

/*
 * The ISensorsWrapper interface includes all function from supported Sensors HAL versions. This
 * allows for the SensorDevice to use the ISensorsWrapper interface to interact with the Sensors
 * HAL regardless of the current version of the Sensors HAL that is loaded. Each concrete
 * instantiation of ISensorsWrapper must correspond to a specific Sensors HAL version. This design
 * is beneficial because only the functions that change between Sensors HAL versions must be newly
 * newly implemented, any previously implemented function that does not change may remain the same.
 *
 * Functions that exist across all versions of the Sensors HAL should be implemented as pure
 * virtual functions which forces the concrete instantiations to implement the functions.
 *
 * Functions that do not exist across all versions of the Sensors HAL should include a default
 * implementation that generates an error if called. The default implementation should never
 * be called and must be overridden by Sensors HAL versions that support the function.
 */
class ISensorsWrapper : public VirtualLightRefBase {
public:
    virtual bool supportsPolling() const = 0;

    virtual bool supportsMessageQueues() const = 0;

    virtual Return<void> getSensorsList(ISensors::getSensorsList_cb _hidl_cb) = 0;

    virtual Return<Result> activate(int32_t sensorHandle, bool enabled) = 0;

    virtual Return<Result> batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                                 int64_t maxReportLatencyNs) = 0;

    virtual Return<void> poll(int32_t maxCount, ISensors::poll_cb _hidl_cb) {
        (void)maxCount;
        (void)_hidl_cb;
        return Return<void>();
    }
};

template<typename T>
class SensorsWrapperBase : public ISensorsWrapper {
public:
    SensorsWrapperBase(sp<T> sensors) :
        mSensors(sensors) { };

    Return<void> getSensorsList(ISensors::getSensorsList_cb _hidl_cb) override {
        return mSensors->getSensorsList(_hidl_cb);
    }

    Return<Result> activate(int32_t sensorHandle, bool enabled) override {
        return mSensors->activate(sensorHandle, enabled);
    }

    Return<Result> batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                         int64_t maxReportLatencyNs) override {
        return mSensors->batch(sensorHandle, samplingPeriodNs, maxReportLatencyNs);
    }

protected:
    sp<T> mSensors;
};

class SensorsWrapperV1_0 : public SensorsWrapperBase<hardware::sensors::V1_0::ISensors> {
public:
    SensorsWrapperV1_0(sp<hardware::sensors::V1_0::ISensors> sensors) :
        SensorsWrapperBase(sensors) { };

    bool supportsPolling() const override {
        return true;
    }

    bool supportsMessageQueues() const override {
        return false;
    }

    Return<void> poll(int32_t maxCount,
                      hardware::sensors::V1_0::ISensors::poll_cb _hidl_cb) override {
        return mSensors->poll(maxCount, _hidl_cb);
    }
};

}; // namespace SensorServiceUtil
}; // namespace android

#endif // ANDROID_SENSORS_WRAPPER_H
