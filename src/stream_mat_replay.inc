// Included inside stream, opt-in dump profile, never negotiated as E-AC3/Opus.
int mat_rtp_replay_diagnostic(int argc,char** argv) {
  using spatial_audio::require;
  try {
    require(argc==3 || argc==4,"MAT replay: input.raw loopback-port plain|aes [session]");
    const bool encrypted=std::string_view(argv[2])=="aes";
    require(encrypted || std::string_view(argv[2])=="plain","invalid encryption mode");
    size_t parsed=0;const std::string port_text(argv[1]);int port=std::stoi(port_text,&parsed);
    require(parsed==port_text.size() && port>=1024 && port<=65535,"loopback port");
    const auto size=std::filesystem::file_size(argv[0]);
    // The existing CBC IV uses 16-bit RTP sequence. Do NOT silently wrap/reuse it.
    // This replay profile is deliberately <=20s; long sessions require rekeying.
    require(size && size%MAT_WIRE_CYCLE==0 && size/MAT_WIRE_CYCLE<=1000,"1..1000 whole MAT cycles required");
    const auto epoch=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())|1;
    spatial_audio::MatReplaySource source(argv[0],epoch,1);
    require(source.start(),"MAT source start");
    reed_solomon_init();asio::io_context io;
    udp::socket sock(io,udp::endpoint(asio::ip::address_v4::loopback(),0));
    session_t replay{};
    // Synthetic TRANSPORT tick for unmodified Moonlight FEC. Actual 48k carrier
    // PTS lives inside each fragment, never inferred from this RTP timestamp.
    replay.config.audio.packetDuration=1;
    replay.config.audio.codec=argc==4?audio::CODEC_MAT_DUMP:-1;
    replay.config.encryptionFlagsEnabled=encrypted?SS_ENC_AUDIO:0;
    replay.localAddress=asio::ip::address_v4::loopback();
    replay.audio.peer=udp::endpoint(replay.localAddress,static_cast<unsigned short>(port));
    crypto::aes_t key(16);for(int i=0;i<16;i++)key[i]=uint8_t(i); // Public fixture key.
    replay.audio.cipher=crypto::cipher::cbc_t{key,true};replay.audio.avRiKeyId=0x10203040;
    constexpr auto capacity=((MAX_AUDIO_PACKET_SIZE/16)+1)*16;
    static_assert(MAT_WIRE_PACKET+16<=capacity);
    replay.audio.shards=util::buffer_t<char>(RTPA_TOTAL_SHARDS*capacity);
    replay.audio.shards_p=util::buffer_t<uint8_t*>(RTPA_TOTAL_SHARDS);
    std::memset(replay.audio.shards.begin(),0,replay.audio.shards.size());
    for(int i=0;i<RTPA_TOTAL_SHARDS;i++)replay.audio.shards_p[i]=reinterpret_cast<uint8_t*>(replay.audio.shards.begin()+i*capacity);
    replay.audio.fec_packet={};replay.audio.fec_packet.rtp.header=0x80;
    replay.audio.fec_packet.rtp.packetType=127;replay.audio.fec_packet.fecHeader.payloadType=97;
    auto packets=mail::man->queue<audio::packet_t>(mail::audio_packets);
    spatial_transport::replay_ack.store(0);
    std::thread sender(audioBroadcastThread,std::ref(sock));
    uint64_t submitted=0,frames=0;uint32_t stream_hash=2166136261u;
    const auto start=std::chrono::steady_clock::now();
    const auto send=[&](const uint8_t* data,size_t n) {
      audio::buffer_t packet(n);std::memcpy(packet.begin(),data,n);
      require(packets->try_raise(&replay,std::move(packet)),"MAT test queue full");++submitted;
      const auto deadline=std::chrono::steady_clock::now()+2s;
      while((argc==4?replay.audio.mat_sent.load(std::memory_order_acquire):spatial_transport::replay_ack.load(std::memory_order_acquire))<submitted) {
        require(std::chrono::steady_clock::now()<deadline,"broadcaster acknowledgement timeout");
        std::this_thread::sleep_for(20us);
      }
      require(!replay.audio.mat_send_failed.load(),"MAT UDP send failed");
    };
    try {
      if(argc==4) {
        auto manifest=spatial_transport::inspect_replay(argv[0]);
        spatial_transport::replay_session(*manifest,send);frames=manifest->profile.cycles;
      } else {
      uint8_t control[MAT_WIRE_PACKET];
      mat_wire_make(control,MAT_WIRE_SYNC,epoch,1,0,0,0,nullptr);
      for(unsigned i=0;i<4;i++)send(control,sizeof(control)); // Moonlight synchronization group.
      const auto media_start=std::chrono::steady_clock::now();
      spatial_audio::Frame frame;
      while(true) {
        auto status=source.read(frame);
        if(status==spatial_audio::Read::ended)break;
        require(status==spatial_audio::Read::frame,"MAT source read failure");
        require(frames<size/MAT_WIRE_CYCLE && frame.sequence==frames,"recording changed or replay limit exceeded");
        unsigned index=0;
        spatial_transport::fragment(frame,[&](const uint8_t* p,size_t n) {
          std::this_thread::sleep_until(media_start+std::chrono::microseconds(frames*20000+uint64_t(index++)*20000/60));
          send(p,n);
        });
        stream_hash=mat_hash_update(stream_hash,frame.bytes.data(),frame.bytes.size());++frames;
      }
      require(frames==size/MAT_WIRE_CYCLE,"recording changed length");
      mat_wire_make(control,MAT_WIRE_END,epoch,1,frames,0,stream_hash,nullptr);
      for(unsigned i=0;i<4;i++)send(control,sizeof(control));
      }
    } catch(...) {
      packets->raise(nullptr,audio::buffer_t{});sender.join();throw;
    }
    packets->raise(nullptr,audio::buffer_t{});sender.join();
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"MAT_SENT cycles="<<frames<<" bytes="<<size<<" dataPackets="<<submitted<<" elapsedMs="<<elapsed<<" maxInFlight=1\n";
    return replay.audio.sequenceNumber==submitted?0:6;
  } catch(const std::exception& e) {std::cerr<<"MAT replay failed: "<<e.what()<<'\n';return 7;}
}
