// An insert pulled out of a strip is not freed on the spot: the audio thread
// may still be inside it, so it waits in a retired list until two renders have
// gone by. This checks the three states that gate has to tell apart.
//
// The one that used to be wrong is the first: with no audio server the app
// stays fully usable (the status bar says so and invites you to restart), but
// the generation only advances at the end of ChannelStrip::process(), which the
// JACK callback drives. With no callback the gate never opened and every plugin
// added and removed was held until the process exited.

#include <cstdio>
#include <memory>
#include <string>

#include "core/channel_strip.h"
#include "core/looper.h"

namespace {

constexpr uint32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int kRounds = 50;

int failures = 0;
int alive = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(int got, int want, const std::string& what) {
  if (got == want) return;
  fail(what + " (got " + std::to_string(got) + ", wanted " +
       std::to_string(want) + ")");
}

// An insert that only counts its own life. Derives from the looper so there is
// no plugin interface to reimplement here.
struct Counted : nirbija::LooperInstance {
  Counted() { ++alive; }
  ~Counted() override { --alive; }
};

// add, remove, reclaim - the loop a player makes auditioning plugins.
void churn(nirbija::ChannelStrip& strip, bool audio_running, bool render) {
  float left[kBlock] = {};
  float right[kBlock] = {};
  float* buffers[2] = {left, right};
  for (int i = 0; i < kRounds; ++i) {
    strip.add_insert(std::make_unique<Counted>());
    strip.remove_insert(0);
    if (render) {
      strip.process(buffers, kBlock, nullptr, 0, nullptr);
      strip.process(buffers, kBlock, nullptr, 0, nullptr);
    }
    strip.reclaim(audio_running);
  }
}

}  // namespace

int main() {
  // No audio server at all: nothing can be inside a retired insert, so the
  // wait has nothing to wait for and everything goes now.
  {
    nirbija::ChannelStrip strip("no audio", 2);
    const int before = alive;
    churn(strip, /*audio_running=*/false, /*render=*/false);
    expect(alive - before, 0, "an insert removed with no audio server is freed");
  }

  // The ordinary case: audio rolling, two renders per round, all freed.
  {
    nirbija::ChannelStrip strip("rolling", 2);
    strip.prepare(kRate, kBlock);
    const int before = alive;
    churn(strip, /*audio_running=*/true, /*render=*/true);
    expect(alive - before, 0, "an insert removed while rendering is freed");
  }

  // And the reason the fix cannot simply be "generation == 0": a strip built
  // while audio is already running also sits at generation 0 until its first
  // block, and the audio thread may be inside one of its inserts by then.
  // Nothing may be freed here.
  {
    nirbija::ChannelStrip strip("audio up, not yet rendered", 2);
    strip.prepare(kRate, kBlock);
    const int before = alive;
    churn(strip, /*audio_running=*/true, /*render=*/false);
    expect(alive - before, kRounds,
           "a strip that has not rendered yet keeps its retired inserts");
  }

  if (failures == 0) std::printf("insert_reclaim: ok\n");
  return failures == 0 ? 0 : 1;
}
