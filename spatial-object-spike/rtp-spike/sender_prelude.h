// State-only frontend for a mechanically extracted, unchanged production
// audioBroadcastThread body. Not a mock RTP/encryption/FEC implementation.
#include "src/stream.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include <fstream>
#include <thread>
#include <rs.h>
extern "C" {
#include "moonlight-common-c/src/Limelight-internal.h"
}
namespace asio = boost::asio;
using asio::ip::udp;
using namespace std::literals;

boost::log::sources::severity_logger<int> verbose(0), debug(1), info(2), warning(3), error(4), fatal(5);

namespace stream {
  struct audio_packet_t { RTP_PACKET rtp; };
  struct audio_fec_packet_t { RTP_PACKET rtp; AUDIO_FEC_HEADER fecHeader; };
  static_assert(sizeof(audio_packet_t)==12 && sizeof(audio_fec_packet_t)==24);
  struct session_t {
    config_t config;
    std::atomic<std::int64_t> last_audio_activity_ms {0};
    boost::asio::ip::address localAddress;
    struct {
      crypto::cipher::cbc_t cipher;
      std::uint16_t sequenceNumber=0;
      std::uint32_t avRiKeyId=0, timestamp=0;
      udp::endpoint peer;
      util::buffer_t<char> shards;
      util::buffer_t<std::uint8_t *> shards_p;
      audio_fec_packet_t fec_packet;
      std::atomic<uint64_t> mat_sent{0};
      std::atomic_bool mat_send_failed{false};
    } audio;
  };
  namespace fec {
    using rs_t=util::safe_ptr<reed_solomon, [](reed_solomon *rs) { reed_solomon_release(rs); }>;
  }
  std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
// Build appends exact production constants, encode_audio, audioBroadcastThread,
// then the bounded fixture feeder and closing namespace.
