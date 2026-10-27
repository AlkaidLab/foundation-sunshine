// Shared object bridge contracts. Generated data only; no native attachment.
#define NOMINMAX
#include "bridge.h"
#include <iostream>
#include <limits>
#include <vector>
static void
Check(bool b) {
  if (!b) throw std::runtime_error("Object bridge assertion");
}
int
main() {
  try {
    auto initial = std::make_unique<sonic_live::Pass>();
    *initial = {};
    initial->renderer = 7;
    initial->firstQpc = initial->lastQpc = 1;
    initial->rate = 48000;
    initial->deviceFrames = 480;
    initial->count = 2;
    for (unsigned i = 0; i < 2; ++i) {
      auto &o = initial->objects[i];
      o.host = 99 + i;
      o.id = 42 + i;
      o.type = 1;
      o.frames = 480;
      o.result = S_OK;
      o.volume = i ? .8f : .3f;
      o.gain = i ? .25f : .5f;
      for (auto &f : o.pcm) f = i ? .1f : .2f;
    }
    auto p = std::make_unique<sonic_live::Pass>(*initial);
    sonic_live::Bridge bridge;
    std::vector<std::unique_ptr<receiver::Slot>> packets;
    auto emit = [&](const receiver::Slot &s) {packets.push_back(std::make_unique<receiver::Slot>(s));return true; };
    bridge.push(*p, emit);
    Check(packets.size() == 1 && packets[0]->header.calls == 2 && packets[0]->calls[0].flags == 7);
    Check(packets[0]->calls[0].pcm[0] == .2f * (.3f * .5f) && packets[0]->calls[1].pcm[0] == .1f * (.8f * .25f));
    p->sequence = 1;
    ++p->firstQpc;
    ++p->lastQpc;
    p->objects[0].ended = 1;
    bridge.push(*p, emit);
    p->sequence = 2;
    p->firstQpc = p->lastQpc + 1;
    p->lastQpc = p->firstQpc;
    p->count = 1;
    p->endCount = 0;
    p->objects[0] = p->objects[1];
    p->objects[0].ended = 1;
    bridge.push(*p, emit);
    Check(packets[2]->header.calls == 2 && packets[2]->calls[0].flags == 8 && packets[2]->calls[1].flags == 6);
    p->sequence = 3;
    ++p->firstQpc;
    ++p->lastQpc;
    p->count = 0;
    bridge.push(*p, emit);
    bridge.finish(emit);
    Check(bridge.sourcePasses == 4 && bridge.emptyPasses == 1 && bridge.inputSamples == 5 * 480 && packets[3]->calls[0].flags == 8 && packets[4]->header.kind == 2);
    {
      sonic_live::Bridge delayed;
      *p = *initial;
      unsigned calls = 0;
      delayed.push(*p, [](auto &) { return true; });
      p->sequence = 1;
      ++p->firstQpc;
      ++p->lastQpc;
      p->count = 0;
      p->endCount = 2;
      p->ends[0] = { 99, 42, 0 };
      p->ends[1] = { 100, 43, 0 };
      delayed.push(*p, [&](auto &packet) {Check(packet.header.calls==2&&packet.calls[0].flags==8&&packet.calls[1].flags==8);++calls;return true; });
      delayed.finish([&](auto &packet) {Check(packet.header.kind==2&&packet.header.calls==0);++calls;return true; });
      Check(calls == 2);
    }
    for (unsigned test = 0; test < 10; ++test) {
      sonic_live::Bridge broken;
      *p = *initial;
      if (test == 0) p->deviceFrames = 448;
      if (test == 1) p->rate = 44100;
      if (test == 2) p->objects[0].offset = 16;
      if (test == 3) p->objects[0].result = S_FALSE;
      if (test == 4) p->objects[0].pcm[7] = std::numeric_limits<float>::quiet_NaN();
      if (test == 5) p->objects[0].type = 1u << 13;
      if (test == 6) p->objects[1] = p->objects[0];
      if (test == 7) p->objects[0].gain = 100;
      bool failed = false;
      try {
        broken.push(*p, [](auto &) { return true; });
        if (test == 8) {
          p->sequence = 1;
          p->count = 0;
          broken.push(*p, [](auto &) { return true; });
        }
        if (test == 9) {
          p->sequence = 1;
          p->renderer++;
          broken.push(*p, [](auto &) { return true; });
        }
      }
      catch (...) {
        failed = true;
      }
      Check(failed);
      failed = false;
      try {
        broken.finish([](auto &) { return true; });
      }
      catch (...) {
        failed = true;
      }
      Check(failed);
    }
    std::cout << "PASS shared bridge PCM/gain/XYZ, EOS retirement, missing-object and 10 fail-closed contracts; nativeCapture=0\n";
    return 0;
  }
  catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
