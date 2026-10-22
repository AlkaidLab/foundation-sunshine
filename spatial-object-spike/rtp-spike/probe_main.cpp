#include "src/globals.h"
#include "src/stream.h"
#include <iostream>

// No real application globals (display/NVIDIA preferences/process manager).
safe::mail_t mail::man;

int main(int argc, char **argv) {
  if (argc != 5 || std::string_view(argv[1]) != "--audio-rtp-replay") {
    std::cerr << "usage: --audio-rtp-replay input.eac3 localhost-port plain|aes\n";
    return 2;
  }
  mail::man = std::make_shared<safe::mail_raw_t>();
  const int result = stream::audio_rtp_replay_diagnostic(argc - 2, argv + 2);
  // The production Windows send path starts an address-change watcher.
  // The isolated process has no application lifecycle to shut it down.
  ExitProcess(static_cast<UINT>(result));
}
