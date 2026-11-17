#pragma once
#include <mutex>

// Protect the complete read-modify-write cycle shared by Sparxie and the
// ordinary applications API. Recursive because route handlers call helpers.
namespace sparxie {
  inline std::recursive_mutex apps_file_mutex;
}
