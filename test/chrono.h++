/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT, VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#pragma once

#include "base.h++"
#include "utils.h++"

namespace chrono {

#pragma pack(push, 4)

typedef union time {
  uint64_t fixedpoint;
  __anonymous_struct_extension__ struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t fractional;
    union {
      uint32_t utc;
      uint32_t integer;
    };
#else
    union {
      uint32_t utc;
      uint32_t integer;
    };
    uint32_t fractional;
#endif
  };

  void reset() { fixedpoint = 0; }
  uint32_t seconds() const { return utc; }
} time;

#pragma pack(pop)

uint32_t ns2fractional(uint32_t);
uint32_t fractional2ns(uint32_t);
uint32_t us2fractional(uint32_t);
uint32_t fractional2us(uint32_t);
uint32_t ms2fractional(uint32_t);
uint32_t fractional2ms(uint32_t);

time from_ns(uint64_t us);
time from_us(uint64_t ns);
time from_ms(uint64_t ms);

inline time from_seconds(uint64_t seconds) {
  assert(seconds < UINT32_MAX);
  time result;
  result.fixedpoint = seconds << 32;
  return result;
}

inline time from_utc(time_t utc) {
  assert(utc >= 0);
  return from_seconds((uint64_t)utc);
}

inline time infinite() {
  time result;
  result.fixedpoint = UINT64_MAX;
  return result;
}

#if defined(HAVE_TIMESPEC_TV_NSEC) || defined(__timespec_defined) || defined(CLOCK_REALTIME)
inline time from_timespec(const struct timespec &ts) {
  time result;
  result.fixedpoint = ((uint64_t)ts.tv_sec << 32) | ns2fractional((uint32_t)ts.tv_nsec);
  return result;
}
#endif /* HAVE_TIMESPEC_TV_NSEC */

#if defined(HAVE_TIMEVAL_TV_USEC) || defined(_STRUCT_TIMEVAL)
inline time from_timeval(const struct timeval &tv) {
  time result;
  result.fixedpoint = ((uint64_t)tv.tv_sec << 32) | us2fractional((uint32_t)tv.tv_usec);
  return result;
}
#endif /* HAVE_TIMEVAL_TV_USEC */

time now_realtime();
time now_monotonic();

} /* namespace chrono */
