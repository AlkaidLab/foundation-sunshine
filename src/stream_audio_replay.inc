// Included inside namespace stream, only in the opt-in diagnostic executable.
// Runs the REAL audio_packets queue -> audioBroadcastThread -> platf::send.
// No RTSP/authentication/video/audio capture and no persistent configuration.
int audio_rtp_replay_diagnostic(int argc, char **argv) {
  try {
    if (argc != 3 || (std::string_view(argv[2]) != "plain" && std::string_view(argv[2]) != "aes")) {
      BOOST_LOG(error) << "usage: --audio-rtp-replay input.eac3 localhost-port plain|aes";
      return 2;
    }
    const std::string port_text(argv[1]);
    std::size_t parsed = 0;
    const int port = std::stoi(port_text, &parsed);
    if (parsed != port_text.size() || port < 1024 || port > 65535) {
      return 2;
    }
    std::ifstream source(argv[0], std::ios::binary);
    if (!source) {
      return 3;
    }
    std::vector<audio::buffer_t> frames;
    while (source.peek() != std::char_traits<char>::eof()) {
      unsigned char h[6];
      if (!source.read(reinterpret_cast<char *>(h), sizeof(h)) ||
          h[0] != 0x0b || h[1] != 0x77 || (h[4] & 0xf0) != 0x30 ||
          (h[5] >> 3) != 16 || (h[2] >> 6) != 0) {
        return 4; // Strictly independent 48 kHz / 1536-sample E-AC3 frames.
      }
      const std::size_t bytes = ((((h[2] & 7) << 8) | h[3]) + 1) * 2;
      if (bytes < sizeof(h) || bytes > MAX_AUDIO_PACKET_SIZE || frames.size() == 2000) {
        return 5;
      }
      audio::buffer_t frame(bytes);
      std::memcpy(frame.begin(), h, sizeof(h));
      if (!source.read(reinterpret_cast<char *>(frame.begin() + sizeof(h)), bytes - sizeof(h))) {
        return 4;
      }
      frames.push_back(std::move(frame));
    }
    if (frames.empty()) {
      return 4;
    }
    reed_solomon_init();
    asio::io_context io;
    udp::socket sock(io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
    session_t replay {};
    replay.config.audio.packetDuration = 32;
    replay.config.audio.codec = audio::CODEC_EAC3;
    const bool encrypted = std::string_view(argv[2]) == "aes";
    replay.config.encryptionFlagsEnabled = encrypted ? SS_ENC_AUDIO : 0;
    replay.localAddress = asio::ip::address_v4::loopback();
    replay.audio.peer = udp::endpoint(replay.localAddress, static_cast<unsigned short>(port));
    // Public test key only, unrelated to any real host/client pairing.
    crypto::aes_t key(16);
    for (int i = 0; i < 16; ++i) {
      key[i] = static_cast<std::uint8_t>(i);
    }
    replay.audio.cipher = crypto::cipher::cbc_t { key, true };
    replay.audio.avRiKeyId = 0x10203040;
    // Allocate a FULL extra AES block: PKCS#7 adds 16 bytes when aligned.
    constexpr auto capacity = ((MAX_AUDIO_PACKET_SIZE / 16) + 1) * 16;
    replay.audio.shards = util::buffer_t<char>(RTPA_TOTAL_SHARDS * capacity);
    replay.audio.shards_p = util::buffer_t<std::uint8_t *>(RTPA_TOTAL_SHARDS);
    std::memset(replay.audio.shards.begin(), 0, replay.audio.shards.size());
    for (int i = 0; i < RTPA_TOTAL_SHARDS; ++i) {
      replay.audio.shards_p[i] = reinterpret_cast<std::uint8_t *>(replay.audio.shards.begin() + i * capacity);
    }
    replay.audio.fec_packet = {};
    replay.audio.fec_packet.rtp.header = 0x80;
    replay.audio.fec_packet.rtp.packetType = 127;
    replay.audio.fec_packet.fecHeader.payloadType = 97;

    auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);
    std::thread sender(audioBroadcastThread, std::ref(sock));
    // Bounded preloaded fixture, one frame every 32 ms. Not live capture.
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < frames.size(); ++i) {
      std::this_thread::sleep_until(start + std::chrono::milliseconds(i * 32));
      packets->raise(&replay, std::move(frames[i]));
    }
    packets->raise(nullptr, audio::buffer_t {});
    sender.join();
    return replay.audio.sequenceNumber == frames.size() ? 0 : 6;
  }
  catch (const std::exception &e) {
    BOOST_LOG(error) << "Audio RTP replay diagnostic: " << e.what();
    return 7;
  }
}
