// Real native pipe only. No file input, generated source, audio device changes,
// attachment, elevation or fallback. This is a loopback transport bring-up check,
// not a hardware playback / full Sunshine RTSP / game A/V sync acceptance test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include "../../src/spatial_transport/mat_live_session.h"
#include "live_check_transport.h"
#include <openssl/evp.h>
#include <deque>
#include <iostream>
#include <iomanip>
#include <sstream>
using spatial_audio::require;
struct Digest {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  Digest() { require(context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1, "SHA256 init"); }
  ~Digest() { EVP_MD_CTX_free(context); }
  void
  update(const uint8_t *p, size_t n) { require(EVP_DigestUpdate(context, p, n) == 1, "SHA256 update"); }
  std::string
  finish() {
    uint8_t hash[32];
    unsigned n;
    require(EVP_DigestFinal_ex(context, hash, &n) == 1 && n == 32, "SHA256 finish");
    std::ostringstream out;
    for (auto b : hash) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    return out.str();
  }
};
struct Proof {
  Digest input, output;
  std::deque<std::string> pending;
  uint64_t source = 0, received = 0, ended = 0;
  LI_MAT_LIVE_PROFILE profile {};
  static int
  cycle(void *opaque, const uint8_t *bytes, int n, uint64_t epoch, uint64_t domain, uint64_t index, uint64_t pts) {
    auto &p = *static_cast<Proof *>(opaque);
    try {
      require(n == 61440 && epoch == p.profile.epoch && domain == p.profile.domain && index == p.received && pts == index * 960, "Client cycle identity/clock");
      require(!p.pending.empty(), "Client emitted a cycle without a source");
      Digest hash;
      hash.update(bytes, n);
      require(hash.finish() == p.pending.front(), "Source/client per-cycle SHA256 differs");
      p.pending.pop_front();
      p.output.update(bytes, n);
      ++p.received;
      return 1;
    }
    catch (const std::exception &e) {
      std::cerr << "CLIENT_FAILED " << e.what() << '\n';
      return 0;
    }
  }
  static int
  end(void *opaque, uint64_t count) {
    auto &p = *static_cast<Proof *>(opaque);
    if (count != p.received || !p.pending.empty()) return 0;
    ++p.ended;
    return 1;
  }
};
struct ObservedSource final: spatial_audio::EncodedAudioSource {
  std::unique_ptr<spatial_audio::EncodedAudioSource> native;
  Proof &proof;
  ObservedSource(std::unique_ptr<spatial_audio::EncodedAudioSource> source, Proof &p): native(std::move(source)), proof(p) {}
  spatial_audio::Descriptor
  descriptor() const override { return native->descriptor(); }
  bool
  start() override { return native->start(); }
  spatial_audio::Read
  read(spatial_audio::Frame &f) override {
    auto status = native->read(f);
    if (status == spatial_audio::Read::frame) {
      require(proof.pending.size() < 4, "Client backlog exceeds four source cycles");
      Digest hash;
      hash.update(f.bytes.data(), f.bytes.size());
      proof.pending.push_back(hash.finish());
      proof.input.update(f.bytes.data(), f.bytes.size());
      ++proof.source;
    }
    else if (status == spatial_audio::Read::failed)
      std::cerr << "SOURCE_FAILED " << native->error() << '\n';
    return status;
  }
  void
  stop() override { native->stop(); }
  spatial_audio::State
  state() const override { return native->state(); }
  std::string
  error() const override { return native->error(); }
};
int
main(int argc, char **argv) {
  Proof proof;
  uint8_t key[16] {};
  HANDLE timer = nullptr;
  try {
    std::string pipe, producer;
    unsigned seconds = 30;
    bool drop = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--pipe" && i + 1 < argc)
        pipe = argv[++i];
      else if (a == "--producer" && i + 1 < argc)
        producer = argv[++i];
      else if (a == "--max-seconds" && i + 1 < argc) {
        auto n = std::stoul(argv[++i]);
        require(n && n <= 60, "Duration must be 1..60 seconds");
        seconds = static_cast<unsigned>(n);
      }
      else if (a == "--drop-for-fec")
        drop = true;
      else
        throw std::runtime_error("Usage: --pipe LIVE_PIPE --producer EXACT_PRODUCER [--max-seconds 1..60] [--drop-for-fec]");
    }
    require(!pipe.empty() && !producer.empty() && seconds && seconds <= 60, "Explicit native source and bounded duration required");
    const auto began = std::chrono::steady_clock::now(), deadline = began + std::chrono::seconds(seconds);
    auto cancelled = [&] { return std::chrono::steady_clock::now() >= deadline; };
    auto session = spatial_transport::prepare_live(pipe, producer);
    require(bool(session), "Native live source unavailable");
    proof.profile = session->profile;
    auto *native = dynamic_cast<spatial_audio::NativeMatPipeSource *>(session->source.get());
    require(native != nullptr, "Only native pipe accepted");
    native->cancel_when(cancelled);
    const auto id = native->identity();
    std::cout << "SOURCE engine=" << id.engine_pid << " created=" << id.engine_created << " publisher=" << id.publisher_pid << " fileInput=0\n";
    session->source = std::make_unique<ObservedSource>(std::move(session->source), proof);
    require(RAND_bytes(key, sizeof(key)) == 1, "Ephemeral session key");
    LI_MAT_PLAYBACK_CALLBACKS cb { Proof::cycle, Proof::end, &proof };
    require(LiveCheckOpen(&proof.profile, key, &cb, drop), "Actual client/UDP loopback init");
    timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    require(timer != nullptr, "High resolution wait timer");
    auto wait = [&](auto delay) {auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();if(ns<=0)return;
            LARGE_INTEGER due{};due.QuadPart=-((ns+99)/100);require(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)&&WaitForSingleObject(timer,1000)==WAIT_OBJECT_0,"Packet pacing timer"); };
    auto count = spatial_transport::live_session(*session, key, sizeof(key), [](const uint8_t *bytes, size_t n) { require(LiveCheckSend(bytes, n), "UDP/RTP/FEC/authentication/client failed"); }, wait, cancelled);
    require(count == proof.source && count == proof.received && proof.ended == 1 && proof.pending.empty(), "End-to-end count/clean EOS");
    auto input = proof.input.finish(), output = proof.output.finish();
    require(input == output, "Whole-stream SHA256 mismatch");
    require(!drop || LiveCheckDrops() == LiveCheckRecovered(), "FEC did not recover every deliberate loss");
    std::cout << "PASS_NATIVE_LOOPBACK cycles=" << count << " bytes=" << count * 61440 << " sha256=" << input
              << " datagrams=" << LiveCheckDatagrams() << " deliberateDrops=" << LiveCheckDrops() << " recovered=" << LiveCheckRecovered()
              << " elapsedSeconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count()
              << " rtpWrapObserved=" << (count * 60 + 8 > 65536) << " authenticatedEOS=1 hardwareSink=0 fullRtspSession=0 avSyncVerified=0\n";
    LiveCheckClose();
    CloseHandle(timer);
    MatLiveWipe(key, sizeof(key));
    return 0;
  }
  catch (const std::exception &e) {
    std::cerr << "FAIL_NATIVE_LOOPBACK " << e.what() << " sourceCycles=" << proof.source << " clientCycles=" << proof.received << " noAcceptance=1\n";
    LiveCheckClose();
    if (timer) CloseHandle(timer);
    MatLiveWipe(key, sizeof(key));
    return 1;
  }
}
