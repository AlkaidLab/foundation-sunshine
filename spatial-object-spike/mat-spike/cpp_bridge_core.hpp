#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../resident/position_cache.h"
#include <array>
#include <bit>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/audio_fifo.h>
}

namespace cpp_bridge {
constexpr unsigned quantum=480,max_source_frames=2048,max_round_objects=37,max_calls=64;
inline void check(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
inline float nan(){return std::bit_cast<float>(uint32_t{0x7fc00000});}
using Position=std::optional<std::array<float,3>>;
using SourceKey=std::tuple<uint64_t,uint32_t,uint32_t>;
struct RecordInput {StateRecord state{};std::array<float,max_source_frames> pcm{};};
struct Batch {uint32_t index{},count{};std::array<RecordInput,max_round_objects> records;};
#pragma pack(push,1)
struct FileHeader {char magic[8];uint32_t version,rate,frames,records;uint64_t qpcHz;};
struct FrameHeader {uint32_t index,frames,calls,flags;int64_t firstQpc,lastQpc;};
struct CallHeader {uint32_t id,slot,type,flags;float xyz[3];uint32_t frames;};
#pragma pack(pop)
static_assert(sizeof(FileHeader)==32 && sizeof(FrameHeader)==32 && sizeof(CallHeader)==32);
struct Span {unsigned offset,frames;Position xyz;};
struct Framed {std::array<float,quantum> pcm;std::vector<Span> spans;bool singlePositionReady() const;};

// Worker-owned PCM FIFO. Preallocated, bounded writes: never invoke FFmpeg's implicit growth.
class Framer {
    struct Pending {unsigned frames;Position xyz;};
    AVAudioFifo* fifo_{};
    std::deque<Pending> metadata_;
public:
    static constexpr int capacity=4096;
    unsigned maxBuffered=0;
    Framer();~Framer();
    Framer(const Framer&)=delete;Framer& operator=(const Framer&)=delete;
    std::vector<Framed> push(std::span<const float> samples,Position xyz);
    void finish() const;
    int buffered() const{return av_audio_fifo_size(fifo_);}
};

class FrameBuffer {
    std::vector<uint8_t> data_;
    FrameHeader header_{};
public:
    FrameBuffer();
    void begin(uint32_t index,int64_t first,int64_t last,bool eof=false);
    void call(uint32_t id,uint32_t slot,uint32_t type,uint32_t flags,Position xyz,std::span<const float> pcm);
    std::span<const uint8_t> bytes() const;
    uint32_t calls() const{return header_.calls;}
};
struct Output {FrameBuffer dynamic,bed;};
struct Identity {uint32_t id,slot;uint64_t host,epoch,generation;uint32_t originalId,type,firstReadyFrame;bool observedCreate;};

class Bridge {
    using Key=std::tuple<uint64_t,uint32_t,uint32_t,uint64_t,uint64_t>;
    struct Active {Key key;uint32_t id,slot;Position xyz;std::unique_ptr<Framer> framer;};
    struct End {uint32_t id,slot;Position xyz;};
    std::map<SourceKey,uint32_t> sourceSlots_;
    std::map<SourceKey,std::unique_ptr<Framer>> bedFramers_;
    std::vector<Active> active_;
    std::vector<End> pending_;
    uint32_t nextId_=1;
    uint64_t epoch_=0;
    int64_t lastFrame_=-1;
    bool closed_=false;
    Output output_;
    void endActive(size_t index,const char* reason);
public:
    PositionCache cache;
    std::map<std::string,uint64_t> counts;
    std::vector<Identity> identities;
    Bridge();
    const Output& consume(const Batch& batch);
    FrameBuffer& finish();
    const std::map<SourceKey,uint32_t>& sourceSlots() const{return sourceSlots_;}
};
FileHeader fileHeader(const char* magic,uint32_t records,uint64_t hz);
SourceKey sourceKey(const RecordInput& record);
} // namespace cpp_bridge
