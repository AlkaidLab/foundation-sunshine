// Offline frontend around UNMODIFIED upstream mat_format.cpp. No driver,
// COM activation, proprietary codec, rendering or audio output is involved.
#include "mat_format.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <array>

int main(int argc, char **argv) {
  try {
    if (argc!=3) return 2;
    std::ifstream input(argv[1],std::ios::binary);
    if (!input) return 3;
    std::vector<BYTE> bytes((std::istreambuf_iterator<char>(input)),{});
    constexpr std::size_t period=61440;
    if (bytes.empty() || bytes.size()>64*1024*1024 || bytes.size()%period) return 4;
    const std::filesystem::path output(argv[2]);
    if (std::filesystem::exists(output) || !std::filesystem::create_directory(output)) return 5;
    std::ofstream pcm(output/"fullband-31ch-48k.pcm16",std::ios::binary);
    std::ofstream csv(output/"fields.csv"); csv<<"half,object,f0,f1,f2,f3,f4,f5\n";
    constexpr std::array<BYTE,6> marker={0x83,0x41,0x43,0xc2,0x08,0x2f};
    std::size_t valid=0,invalid=0;
    for (std::size_t off=0;off<bytes.size();off+=period) {
      const BYTE *b=bytes.data()+off;
      if (b[0]!=0x72 || b[1]!=0xf8 || b[2]!=0x1f || b[3]!=0x4e || (b[4]&31)!=22) return 6;
      const std::size_t length=b[6]|(b[7]<<8);
      if (length%2 || length>period-8) return 6;
      auto logical=dolby::UnswapMatTransportWords(b+8,length);
      auto slots=dolby::FindBytePattern(logical,marker);
      auto tables=dolby::FindBytePattern(logical,dolby::kMatPositionTablePrefix);
      bool okay=slots.size()==62 && tables.size()==2;
      for (auto p:slots) if (p+6+960>logical.size()) okay=false;
      for (auto p:tables) if (p+dolby::kMatPositionTablePrefix.size()+90>logical.size()) okay=false;
      if (!okay) { ++invalid; continue; }
      ++valid;
      for (std::size_t half=0;half<2;++half) {
        for (std::size_t sample=0;sample<480;++sample) for (std::size_t slot=0;slot<31;++slot) {
          const auto p=slots[half*31+slot]+6+sample*2;
          pcm.put(static_cast<char>(logical[p+1]));pcm.put(static_cast<char>(logical[p]));
        }
        for (std::size_t object=0;object<20;++object) {
          csv<<(off/period*2+half)<<','<<object;
          for (auto value:dolby::DecodeMatObjectFields(logical,tables[half],object)) csv<<','<<value;
          csv<<'\n';
        }
      }
    }
    if (!pcm || !csv) return 7;
    std::cout<<"{\"observed_valid_bursts\":"<<valid<<",\"observed_invalid_bursts\":"<<invalid<<"}\n";
    return valid?0:8;
  } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 9;}
}
