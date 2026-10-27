/* Diagnostic output only. No Android, HDMI, decoder or device capability claim.
 * Callbacks copy into a bounded queue. The caller polls a non-blocking sink.
 * Times are monotonic milliseconds; sink positions are unwrapped carrier frames. */
#pragma once
#include "MatReceiver.h"
#include <windows.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

enum { MOCK_CYCLE_BYTES = 61440,
  MOCK_FRAME_BYTES = 16,
  MOCK_QUEUE_CYCLES = 4 };
typedef struct {
  int route, hdmi, mat, direct_iec;
} MockCapabilities;
static inline int
mock_preferred_codec(MockCapabilities c, int strict) {
  int eligible = c.route > 0 && c.hdmi && c.mat && c.direct_iec;
  return eligible ? (strict ? 7 : 6) : (strict ? -1 : 0);
}
typedef struct {
  void *context;
  int (*write)(void *, const uint8_t *, int); /* bytes, frame-aligned; zero means busy */
  uint64_t (*played)(void *);
  int (*route)(void *); /* zero when the required capabilities disappear */
  void (*close)(void *);
} MockSink;
enum MockOutputState { MOCK_RUNNING,
  MOCK_DRAINING,
  MOCK_COMPLETED,
  MOCK_FAILED,
  MOCK_STOPPED };
typedef struct {
  enum MockOutputState state;
  uint64_t accepted, written, submitted_bytes, played_frames, peak_backlog_bytes;
  unsigned queued;
  const char *error;
} MockOutputStats;
typedef struct {
  SRWLOCK lock;
  MockSink sink;
  MockOutputStats stats;
  uint64_t epoch, domain, last_poll, last_write, last_head_change, drain_deadline;
  unsigned stall_ms, drain_ms, read, offset;
  int expected_route, closed;
  uint8_t queue[MOCK_QUEUE_CYCLES][MOCK_CYCLE_BYTES];
} MockOutput;
static inline void
mock_output_fail_locked(MockOutput *p, const char *error) {
  if (p->stats.state != MOCK_FAILED && p->stats.state != MOCK_STOPPED) {
    p->stats.state = MOCK_FAILED;
    p->stats.error = error;
  }
}
static inline MockOutput *
mock_output_open(MockSink sink, int route, uint64_t epoch, uint64_t domain,
  uint64_t now, unsigned stall_ms, unsigned drain_ms) {
  if (!sink.write || !sink.played || !sink.route || !sink.close || route <= 0 || !epoch || !domain || !stall_ms || !drain_ms || sink.route(sink.context) != route) return NULL;
  MockOutput *p = (MockOutput *) calloc(1, sizeof(*p));
  if (!p) return NULL;
  InitializeSRWLock(&p->lock);
  p->sink = sink;
  p->expected_route = route;
  p->epoch = epoch;
  p->domain = domain;
  p->last_poll = p->last_write = p->last_head_change = now;
  p->stall_ms = stall_ms;
  p->drain_ms = drain_ms;
  p->stats.state = MOCK_RUNNING;
  p->stats.error = "";
  return p;
}
static inline int
mock_output_offer(MockOutput *p, const uint8_t *bytes, int n, uint64_t epoch, uint64_t domain,
  uint64_t index, uint64_t pts, uint64_t now) {
  if (!p) return 0;
  AcquireSRWLockExclusive(&p->lock);
  int okay = 0;
  if (p->stats.state != MOCK_RUNNING) {
    if (p->stats.state == MOCK_DRAINING || p->stats.state == MOCK_COMPLETED) mock_output_fail_locked(p, "input after EOS");
    goto done;
  }
  if (n != MOCK_CYCLE_BYTES || !MatCarrierIsValid(bytes, (size_t) n) || epoch != p->epoch || domain != p->domain || index != p->stats.accepted || pts != index * 960) {
    mock_output_fail_locked(p, "cycle identity/format");
    goto done;
  }
  if (p->stats.queued == MOCK_QUEUE_CYCLES) {
    mock_output_fail_locked(p, "output queue full");
    goto done;
  }
  if (!p->stats.queued) p->last_write = now;
  memcpy(p->queue[(p->read + p->stats.queued) % MOCK_QUEUE_CYCLES], bytes, MOCK_CYCLE_BYTES);
  ++p->stats.queued;
  ++p->stats.accepted;
  okay = 1;
done:
  ReleaseSRWLockExclusive(&p->lock);
  return okay;
}
static inline int
mock_output_end(MockOutput *p, uint64_t count, uint64_t now) {
  if (!p) return 0;
  AcquireSRWLockExclusive(&p->lock);
  int okay = 0;
  if (p->stats.state == MOCK_RUNNING) {
    if (!count || count != p->stats.accepted)
      mock_output_fail_locked(p, "EOS cycle count");
    else {
      p->stats.state = MOCK_DRAINING;
      p->drain_deadline = now + p->drain_ms;
      okay = 1;
    }
  }
  ReleaseSRWLockExclusive(&p->lock);
  return okay;
}
static inline MockOutputStats
mock_output_poll(MockOutput *p, uint64_t now) {
  AcquireSRWLockExclusive(&p->lock);
  if (p->stats.state != MOCK_RUNNING && p->stats.state != MOCK_DRAINING) goto done;
  if (now < p->last_poll) {
    mock_output_fail_locked(p, "clock reversed");
    goto done;
  }
  p->last_poll = now;
  if (p->sink.route(p->sink.context) != p->expected_route) {
    mock_output_fail_locked(p, "output route/capability changed");
    goto done;
  }
  /* A fixed budget bounds each poll even when the sink accepts tiny writes. */
  for (unsigned budget = 0; budget < 16 && p->stats.queued; ++budget) {
    int remaining = MOCK_CYCLE_BYTES - (int) p->offset;
    int n = p->sink.write(p->sink.context, p->queue[p->read] + p->offset, remaining);
    if (n < 0 || n > remaining || n % MOCK_FRAME_BYTES) {
      mock_output_fail_locked(p, "sink write failed/invalid");
      goto done;
    }
    if (!n) {
      if (now - p->last_write > p->stall_ms) mock_output_fail_locked(p, "sink write stalled");
      break;
    }
    if (p->stats.submitted_bytes == p->stats.played_frames * MOCK_FRAME_BYTES) p->last_head_change = now;
    p->offset += (unsigned) n;
    p->stats.submitted_bytes += (unsigned) n;
    p->last_write = now;
    if (p->offset == MOCK_CYCLE_BYTES) {
      p->offset = 0;
      p->read = (p->read + 1) % MOCK_QUEUE_CYCLES;
      --p->stats.queued;
      ++p->stats.written;
    }
  }
  if (p->stats.state == MOCK_FAILED) goto done;
  {
    uint64_t head = p->sink.played(p->sink.context);
    if (head < p->stats.played_frames || head > p->stats.submitted_bytes / MOCK_FRAME_BYTES) {
      mock_output_fail_locked(p, "sink playhead invalid");
      goto done;
    }
    if (head > p->stats.played_frames) {
      p->stats.played_frames = head;
      p->last_head_change = now;
    }
    else if (p->stats.submitted_bytes > head * MOCK_FRAME_BYTES && now - p->last_head_change > p->stall_ms) {
      mock_output_fail_locked(p, "sink playhead stalled");
      goto done;
    }
    uint64_t backlog = (uint64_t) p->stats.queued * MOCK_CYCLE_BYTES - p->offset + p->stats.submitted_bytes - head * MOCK_FRAME_BYTES;
    if (backlog > p->stats.peak_backlog_bytes) p->stats.peak_backlog_bytes = backlog;
    if (p->stats.state == MOCK_DRAINING) {
      if (!p->stats.queued && head * MOCK_FRAME_BYTES == p->stats.submitted_bytes && p->stats.written == p->stats.accepted)
        p->stats.state = MOCK_COMPLETED;
      else if (now > p->drain_deadline)
        mock_output_fail_locked(p, "EOS drain timed out");
    }
  }
done:
  if (p->stats.state != MOCK_RUNNING && p->stats.state != MOCK_DRAINING && !p->closed) {
    p->sink.close(p->sink.context);
    p->closed = 1;
  }
  MockOutputStats stats = p->stats;
  ReleaseSRWLockExclusive(&p->lock);
  return stats;
}
static inline void
mock_output_stop(MockOutput *p) {
  if (!p) return;
  AcquireSRWLockExclusive(&p->lock);
  if (p->stats.state == MOCK_RUNNING || p->stats.state == MOCK_DRAINING) p->stats.state = MOCK_STOPPED;
  if (!p->closed) {
    p->sink.close(p->sink.context);
    p->closed = 1;
  }
  ReleaseSRWLockExclusive(&p->lock);
}
static inline void
mock_output_free(MockOutput *p) {
  mock_output_stop(p);
  free(p);
}
static inline MockOutputStats
mock_output_snapshot(MockOutput *p) {
  AcquireSRWLockShared(&p->lock);
  MockOutputStats s = p->stats;
  ReleaseSRWLockShared(&p->lock);
  return s;
}
/* The common-c END callback may return success only after the main poller drains.
 * Stop/failure wakes this bounded wait without touching the device on this thread. */
static inline int
mock_output_wait_end(MockOutput *p) {
  uint64_t deadline = GetTickCount64() + p->drain_ms + 20;
  for (;;) {
    MockOutputStats s = mock_output_snapshot(p);
    if (s.state != MOCK_DRAINING) return s.state == MOCK_COMPLETED;
    if (GetTickCount64() > deadline) {
      AcquireSRWLockExclusive(&p->lock);
      if (p->stats.state == MOCK_DRAINING) mock_output_fail_locked(p, "EOS consumer wait timed out");
      int okay = p->stats.state == MOCK_COMPLETED;
      ReleaseSRWLockExclusive(&p->lock);
      return okay;
    }
    Sleep(1);
  }
}

/* A finite FIFO consuming at a controlled rate. Both written and consumed bytes
 * are hashed; advancing a playhead never manufactures bytes or decodes MAT. */
typedef struct {
  MockCapabilities capabilities;
  uint8_t fifo[2 * MOCK_CYCLE_BYTES];
  unsigned read, queued, max_write, speed_percent, write_calls, zero_every;
  uint64_t now, last_tick, zero_until, fail_after, written, played;
  int busy, closed, invalid_write;
  EVP_MD_CTX *write_digest, *play_digest;
  char written_sha[65], played_sha[65];
} MockDevice;
static inline void
mock_hex_digest(EVP_MD_CTX *digest, char out[65]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t bytes[32];
  unsigned n = 0;
  if (!digest || !EVP_DigestFinal_ex(digest, bytes, &n) || n != 32) {
    out[0] = 0;
    return;
  }
  for (unsigned i = 0; i < 32; ++i) {
    out[2 * i] = hex[bytes[i] >> 4];
    out[2 * i + 1] = hex[bytes[i] & 15];
  }
  out[64] = 0;
}
static inline void
mock_device_close(void *context) {
  MockDevice *d = (MockDevice *) context;
  if (d->closed) return;
  d->closed = 1;
  mock_hex_digest(d->write_digest, d->written_sha);
  mock_hex_digest(d->play_digest, d->played_sha);
  EVP_MD_CTX_destroy(d->write_digest);
  EVP_MD_CTX_destroy(d->play_digest);
  d->write_digest = d->play_digest = NULL;
}
static inline int
mock_device_init(MockDevice *d, MockCapabilities capabilities, uint64_t now) {
  memset(d, 0, sizeof(*d));
  d->capabilities = capabilities;
  d->now = d->last_tick = now;
  d->max_write = 8192;
  d->speed_percent = 100;
  d->write_digest = EVP_MD_CTX_create();
  d->play_digest = EVP_MD_CTX_create();
  if (!d->write_digest || !d->play_digest || !EVP_DigestInit_ex(d->write_digest, EVP_sha256(), NULL) || !EVP_DigestInit_ex(d->play_digest, EVP_sha256(), NULL)) {
    mock_device_close(d);
    return 0;
  }
  return 1;
}
static inline int
mock_device_write(void *context, const uint8_t *bytes, int count) {
  MockDevice *d = (MockDevice *) context;
  if (d->closed || (d->fail_after && d->written >= d->fail_after)) return -1;
  if (d->invalid_write) return 1;
  ++d->write_calls;
  if (d->busy || d->now < d->zero_until || (d->zero_every && d->write_calls % d->zero_every == 0)) return 0;
  unsigned n = (unsigned) count, available = (unsigned) sizeof(d->fifo) - d->queued;
  if (n > available) n = available;
  if (n > d->max_write) n = d->max_write;
  if (d->fail_after && n > d->fail_after - d->written) n = (unsigned) (d->fail_after - d->written);
  n -= n % MOCK_FRAME_BYTES;
  for (unsigned offset = 0; offset < n;) {
    unsigned at = (d->read + d->queued) % (unsigned) sizeof(d->fifo), part = (unsigned) sizeof(d->fifo) - at;
    if (part > n - offset) part = n - offset;
    memcpy(d->fifo + at, bytes + offset, part);
    d->queued += part;
    offset += part;
  }
  if (n && !EVP_DigestUpdate(d->write_digest, bytes, n)) return -1;
  d->written += n;
  return (int) n;
}
static inline void
mock_device_tick(MockDevice *d, uint64_t now) {
  if (d->closed || now < d->last_tick) return;
  uint64_t bytes = (now - d->last_tick) * 3072 * d->speed_percent / 100;
  d->now = d->last_tick = now;
  if (bytes > d->queued) bytes = d->queued;
  bytes -= bytes % MOCK_FRAME_BYTES;
  while (bytes) {
    unsigned part = (unsigned) sizeof(d->fifo) - d->read;
    if (part > bytes) part = (unsigned) bytes;
    EVP_DigestUpdate(d->play_digest, d->fifo + d->read, part);
    d->read = (d->read + part) % (unsigned) sizeof(d->fifo);
    d->queued -= part;
    d->played += part;
    bytes -= part;
  }
}
static inline uint64_t
mock_device_played(void *context) { return ((MockDevice *) context)->played / MOCK_FRAME_BYTES; }
static inline int
mock_device_route(void *context) {
  MockDevice *d = (MockDevice *) context;
  return mock_preferred_codec(d->capabilities, 1) == 7 ? d->capabilities.route : 0;
}
static inline MockSink
mock_device_sink(MockDevice *d) {
  MockSink s = { d, mock_device_write, mock_device_played, mock_device_route, mock_device_close };
  return s;
}
