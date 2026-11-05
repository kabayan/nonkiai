#pragma once
#include <cmath>
namespace word23_f2_detail {
// For finite PCM-derived FFT values, multiply by (1,-0) is exact. Keep the
// original signed-zero subtraction/addition rather than dropping both zeros.
inline void twiddle_zero(double xr, double xi, double &tr, double &ti) {
    tr=xr-std::copysign(0.0,-xi);
    ti=xi+std::copysign(0.0,-xr);
}
}
