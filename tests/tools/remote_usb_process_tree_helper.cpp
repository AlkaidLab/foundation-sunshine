#include <boost/process/v1.hpp>

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

int main(int argc, char **argv) {
  using namespace std::chrono_literals;
  namespace bp = boost::process::v1;

  if (argc == 2 && std::string_view(argv[1]) == "--hold-pipes") {
    std::this_thread::sleep_for(30s);
    return 0;
  }
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "detach") {
      return 0;
    }
  }

  /* The descendant inherits stdout/stderr, then outlives this direct child.
   * run_process must terminate the surrounding process group before joining
   * its pipe readers or the controller test will wait for the full 30 seconds. */
  bp::child holder(argv[0], "--hold-pipes");
  holder.detach();
  std::cout << "7\n" << std::flush;
  return 0;
}
