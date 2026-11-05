// SPDX-License-Identifier: MIT
// Original compatibility wrapper using FusionAhrs and FusionMath (MIT).
#include "madgwick_ahrs.h"

#include <cmath>

MadgwickAhrs::MadgwickAhrs() { reset(); }

void MadgwickAhrs::begin(float sample_frequency_hz)
{
    if (sample_frequency_hz > 1.0f) {
        inv_sample_frequency_ = 1.0f / sample_frequency_hz;
        FusionAhrsSetSamplePeriod(&ahrs_, inv_sample_frequency_);
    }
}

void MadgwickAhrs::set_sample_period(float seconds)
{
    // Preserve the detector's bounded period validation.
    if (seconds >= 0.0001f && seconds <= 0.1f) {
        inv_sample_frequency_ = seconds;
        FusionAhrsSetSamplePeriod(&ahrs_, seconds);
    }
}

void MadgwickAhrs::reset()
{
    FusionAhrsInitialise(&ahrs_);
    FusionAhrsSettings settings = fusionAhrsDefaultSettings;
    settings.sampleRate = 1.0f / inv_sample_frequency_;
    settings.convention = FusionConventionNwu;
    // Fusion correction scales with sin(error), weakening near zero error.
    // The default gain .5 bounds steady gravity error from gyroscope bias.
    // motion_detector supplies zero for untrusted acceleration or pending resync.
    settings.gain = 0.5f;
    // motion_detector already gates untrusted acceleration before this update.
    // A second angle gate could lock out genuine slow pose changes.
    settings.accelerationRejection = 0.0f;
    settings.magneticRejection = 0.0f;
    settings.rejectionTimeout = 0.0f;
    // Disable overrange restart so it cannot discard the seeded pose.
    settings.gyroscopeRange = 0.0f;
    FusionAhrsSetSettings(&ahrs_, &settings);
    // NoMagnetometer removes heading only during startup; preserve seeded yaw.
    FusionAhrsSkipStartup(&ahrs_);
}

void MadgwickAhrs::reset_from_gravity(float ax, float ay, float az)
{
    reset();
    const FusionVector input = {{ax, ay, az}};
    const float norm = FusionVectorNorm(input);
    if (!(norm > 1.0e-9f) || !std::isfinite(norm)) {
        return;
    }
    const FusionVector direction = FusionVectorNormalise(input);
    const float x = direction.axis.x;
    const float y = direction.axis.y;
    const float z = direction.axis.z;
    const float horizontal = std::hypot(x, y);
    FusionQuaternion seed = {{1.0f, 0.0f, 0.0f, 0.0f}};
    if (horizontal > 0.0f) {
        // Axis is gravity cross +Z. atan2 avoids cancellation in 1+z near -Z,
        // retaining the measured tilt even arbitrarily close to antipodal.
        const float half_angle = 0.5f * std::atan2(horizontal, z);
        const float sine = std::sin(half_angle);
        seed = FusionQuaternion{{std::cos(half_angle),
                                 sine * (y / horizontal),
                                 -sine * (x / horizontal), 0.0f}};
    } else if (z < 0.0f) {
        // Exactly antipodal: any horizontal axis works; choose sensor X.
        seed = FusionQuaternion{{0.0f, 1.0f, 0.0f, 0.0f}};
    }
    FusionAhrsSetQuaternion(&ahrs_, FusionQuaternionNormalise(seed));
}

void MadgwickAhrs::update_imu(float gx, float gy, float gz,
                              float ax, float ay, float az)
{
    const FusionVector gyroscope = {{gx, gy, gz}};
    const FusionVector accelerometer = {{ax, ay, az}};
    FusionAhrsUpdateNoMagnetometer(&ahrs_, gyroscope, accelerometer);
}

void MadgwickAhrs::quaternion(float out[4]) const
{
    const FusionQuaternion q = FusionAhrsGetQuaternion(&ahrs_);
    for (int i = 0; i < 4; ++i) out[i] = q.array[i];
}

void MadgwickAhrs::gravity_sensor(float out[3]) const
{
    const FusionVector gravity = FusionAhrsGetGravity(&ahrs_);
    for (int i = 0; i < 3; ++i) out[i] = gravity.array[i];
}

void MadgwickAhrs::rotate_sensor_to_world(float x, float y, float z,
                                          float *out_x, float *out_y, float *out_z) const
{
    const FusionVector sensor = {{x, y, z}};
    const FusionVector earth = FusionMatrixMultiply(
        FusionQuaternionToMatrix(FusionAhrsGetQuaternion(&ahrs_)), sensor);
    *out_x = earth.axis.x;
    *out_y = earth.axis.y;
    *out_z = earth.axis.z;
}
