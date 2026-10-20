#include "core/backends/anan/AnanAudioMixer.h"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace AetherSDR::anan;
void check(bool ok, const char* reason)
{
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}
int main()
{
    AnanAudioMixer mix;
    mix.configure({{0}, {1}});
    const std::vector<float> zero(2, 0.0f);
    mix.push(0, zero);
    check(mix.drain().size() == 2, "an unstarted receiver cannot stall existing audio");
    mix.push(1, zero);
    check(mix.drain().empty(), "joined receiver waits for its peer");
    mix.push(0, zero);
    check(mix.drain().size() == 2, "initial pair drains once");
    std::size_t total = 0;
    for (int round = 0; round < 1000; ++round) {
        const std::size_t frames = 1 + (round * 37) % 512;
        const std::vector<float> a(2 * frames, 0.1f);
        mix.push(0, a);
        check(mix.drain().empty(), "a late peer never becomes fabricated silence");
        const std::size_t first = frames / 2;
        std::vector<float> b(2 * first, 0.2f);
        if (first) {
            mix.push(1, b);
            const auto out = mix.drain();
            check(out.size() == 2 * first, "only matching frames leave the mixer");
            for (float sample : out) {
                check(std::abs(sample - 0.3f) < 1e-6, "both receivers contribute to every output");
            }
        }
        b.assign(2 * (frames - first), 0.2f);
        mix.push(1, b);
        const auto tail = mix.drain();
        check(tail.size() == 2 * (frames - first), "unequal blocks preserve their remaining samples");
        for (float sample : tail) {
            check(std::abs(sample - 0.3f) < 1e-6, "sample pairing survives burst delivery");
        }
        total += frames;
        check(mix.drain().empty(), "no input means no output, regardless of timer activity");
    }
    check(total > 200000 && mix.droppedFrames() == 0, "jitter test conserves all audio frames");
    mix.configure({{0, true}, {1, false, 100, 100}});
    mix.push(0, std::vector<float>(8, 0.4f));
    mix.push(1, std::vector<float>(8, 0.2f));
    auto out = mix.drain();
    check(out.size() == 8 && out[0] == 0 && std::abs(out[1] - 0.2f) < 1e-6,
          "mute and balance apply without changing clock membership");
    mix.push(1, zero);
    check(mix.drain().empty(), "remaining member waits before removal");
    mix.configure({{1}});
    check(mix.drain().size() == 2, "explicit withdrawal releases the surviving receiver");
    check(!mix.push(0, zero), "retired identity cannot rejoin implicitly");
    mix.resetBuffers();
    check(mix.drain().empty(), "rate transition clears queued audio and first-block membership");
    mix.push(1, std::vector<float>(2 * (AnanAudioMixer::kCapacityFrames + 7), 0.1f));
    check(mix.droppedFrames() == 7 && mix.drain().size() == 2 * AnanAudioMixer::kCapacityFrames,
          "overflow is bounded and counted in frames");
    mix.configure({{1}});
    mix.push(1, std::vector<float>{2.0f, -2.0f});
    check(mix.drain() == std::vector<float>({1.0f, -1.0f}), "summed output clips only at final output");
    mix.reset();
    check(mix.droppedFrames() == 0 && !mix.push(1, zero), "disconnect clears membership and diagnostics");
    std::cout << "ANAN sample-driven audio mixer checks passed\n";
}
