# Fusion AHRS dependency

Source: https://github.com/xioTechnologies/Fusion
Commit: a8d7224f36a0ec82345ef49a3db50e65f8d3bab8
License: MIT (see LICENSE.md).

Only FusionAhrs.c and its required headers are vendored, unchanged.
FUSION_USE_NORMAL_SQRT is defined by the build and before wrapper includes,
using the configuration mechanism documented in FusionConfig.h.
The wrapper uses NWU sensor-to-earth quaternions, skips startup, and seeds
orientation explicitly. Gain is 0.2; acceleration rejection, rejection timeout,
and gyroscope range recovery are disabled. The detector supplies acceleration
only when its existing gate accepts it; zero acceleration gives gyro-only updates.
