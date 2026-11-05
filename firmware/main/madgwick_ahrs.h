// SPDX-License-Identifier: MIT
// Original compatibility wrapper using the MIT-licensed Fusion AHRS and math.
#pragma once

#ifndef FUSION_USE_NORMAL_SQRT
#define FUSION_USE_NORMAL_SQRT
#endif
extern "C" {
#include "FusionAhrs.h"
}

class MadgwickAhrs {
public:
    MadgwickAhrs();
    void begin(float sample_frequency_hz);
    void set_sample_period(float seconds);
    float sample_period() const { return inv_sample_frequency_; }
    void reset();
    // Seed the sensor-to-earth rotation taking sensor gravity to earth +Z.
    void reset_from_gravity(float ax, float ay, float az);
    // Gyroscope in deg/s, acceleration in any common unit; zero is gyro-only.
    void update_imu(float gx, float gy, float gz, float ax, float ay, float az);
    void quaternion(float out[4]) const;
    void gravity_sensor(float out[3]) const;
    void rotate_sensor_to_world(float x, float y, float z,
                                float *out_x, float *out_y, float *out_z) const;
private:
    FusionAhrs ahrs_{};
    float inv_sample_frequency_ = 1.0f / 400.0f;
};
