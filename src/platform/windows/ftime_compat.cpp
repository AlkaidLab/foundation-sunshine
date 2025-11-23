#include <sys/timeb.h>

#if defined(_WIN32) && !defined(_MSC_VER)
extern "C" void ftime64(struct __timeb64* timeptr) {
  _ftime64(timeptr);
}
#endif




