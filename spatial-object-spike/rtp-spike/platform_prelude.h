// Build appends exact production addressing/priority/WSASendMsg functions.
#include <boost/asio.hpp>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include "src/platform/common.h"
using namespace std::literals;
namespace platf {
