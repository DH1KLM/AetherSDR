// ASIO format, FIFO and settings checks without loading a real audio driver.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AsioOutput.h"
#include "core/AsioPlaybackBuffer.h"
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

using namespace AetherSDR;
static void check(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
// A 40 ms delayed 10 ms refill is shorter than the playback safety prefill.
static void delayedRefills()
{
    for (const int rate : {44100, 48000, 96000, 192000}) {
        for (const int quantum : {64, 128, 256, 1024}) {
            const std::size_t capacity = AsioPlaybackBuffer::playbackCapacity(rate, quantum);
            AsioPlaybackBuffer ring(capacity, capacity / 2, static_cast<std::size_t>(rate / 200));
            int pending = 0;
            const int refillFrames = rate / 100;
            for (int time = 0; time < 2 * rate; ++time) {
                if (time % refillFrames == 0) { pending += refillFrames; }
                if (time % refillFrames == 0 && !(time > rate / 10 && time < rate * 14 / 100)) {
                    std::vector<float> pcm(static_cast<std::size_t>(pending) * 2);
                    for (int i = 0; i < pending; ++i) { pcm[i*2] = 0.5f; pcm[i*2+1] = -0.25f; }
                    pending -= static_cast<int>(ring.write(pcm));
                }
                if (time % quantum != 0) { continue; }
                for (int frame = 0; frame < quantum; ++frame) {
                    const auto sample = ring.read();
                    if (time >= rate / 10) {
                        check(sample == std::array<float,2>{0.5f,-0.25f},
                              "ASIO scheduling jitter must not insert zeros into continuous PCM");
                    }
                }
            }
            check(ring.underruns() == 0, "no underrun during a delayed refill at any supported rate/quantum");
        }
    }
}
static void prefillAndRecovery()
{
    AsioPlaybackBuffer ring(32, 16, 4);
    const std::array<float,2> frame{0.8f,-0.4f};
    for (int i = 0; i < 15; ++i) {
        check(ring.write(frame) == 1 && ring.read() == std::array<float,2>{0,0},
              "startup preserves short bursts until prefill is complete");
    }
    ring.write(frame);
    for (int i = 1; i <= 16; ++i) {
        const auto actual = ring.read();
        const float gain = std::min(1.0f, i / 4.0f);
        check(actual == std::array<float,2>{frame[0]*gain,frame[1]*gain}, "prefill fades into stereo PCM");
    }
    for (int i = 3; i >= 0; --i) {
        const auto actual = ring.read();
        const float gain = i / 4.0f;
        check(actual == std::array<float,2>{frame[0]*gain,frame[1]*gain}, "a real starvation fades smoothly to silence");
    }
    check(ring.underruns() == 1, "one dry interval counts once");
    for (int i = 0; i < 15; ++i) {
        ring.write(frame);
        check(ring.read() == std::array<float,2>{0,0}, "partial recovery waits instead of chattering between PCM and silence");
    }
    ring.write(frame);
    check(ring.read() == std::array<float,2>{0.2f,-0.1f}, "recovery resumes with a fade-in");
    ring.reset();
    check(ring.read() == std::array<float,2>{0,0}, "source retirement also rearms the prefill");
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("asio-output-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    check(!AsioOutputConfig::load().enabled, "ASIO must be opt-in");
    AsioOutputConfig config;
    config.enabled = true;
    config.driver = QStringLiteral("{00000000-0000-0000-0000-000000000000}");
    config.firstChannel = 4;
    config.sampleRate = 96000;
    config.bufferFrames = 256;
    check(config.save(), "save the whole ASIO config");
    const AsioOutputConfig loaded = AsioOutputConfig::load();
    check(loaded.enabled && loaded.driver == config.driver && loaded.firstChannel == 4
          && loaded.sampleRate == 96000 && loaded.bufferFrames == 256, "restore all ASIO fields");
    config.sampleRate = 12345;
    check(!config.save() && AsioOutputConfig::load().sampleRate == 96000, "refuse unsupported rate without mutation");
    config.sampleRate = 48000;
    config.firstChannel = 255;
    check(!config.valid(), "stereo pair must fit channel bound");
    config.firstChannel = 0;
    AsioOutput output;
    QString error;
    check(!output.start(config, error) && !error.isEmpty() && !output.isOpen(), "unknown driver fails closed");
    output.stop();
    check(!output.flush() && output.bytesFree() == 0, "stopped ASIO has no writable buffer");
    AppSettings::instance().setValue("AsioOutput", QStringLiteral("{\"version\":2,\"enabled\":true}"));
    AppSettings::instance().save();
    check(!config.save() && !AsioOutputConfig::load().enabled, "future config is neither overwritten nor enabled");

    for (const auto [type, bits, bytes] : {std::array{16,16,2}, {17,24,3}, {18,32,4},
            {24,16,4}, {25,18,4}, {26,20,4}, {27,24,4}}) {
        for (const float sample : {-2.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f}) {
            std::array<std::uint8_t, 10> encoded{};
            encoded.fill(0x55);
            AsioPlaybackBuffer::encode(sample, type, encoded.data() + 1);
            check(encoded[0] == 0x55 && encoded[bytes + 1] == 0x55, "conversion respects buffer bounds");
            std::uint32_t word = 0;
            for (int i = 0; i < bytes; ++i) { word |= std::uint32_t{encoded[i + 1]} << (8 * i); }
            if (bytes < 4 && (word & (1u << (bytes * 8 - 1)))) { word |= ~((1u << (bytes * 8)) - 1); }
            const std::int64_t scale = std::int64_t{1} << (bits - 1);
            const std::int64_t expected = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(std::llround(std::clamp<double>(sample, -1, 1) * scale)), -scale, scale - 1);
            check(std::bit_cast<std::int32_t>(word) == expected, "signed little-endian PCM, clipping and aligned depth");
        }
    }
    for (const int type : {16,17,18,19,20,24,25,26,27}) {
        for (const float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            std::array<std::uint8_t, 8> bytes{};
            bytes.fill(0xff);
            AsioPlaybackBuffer::encode(invalid, type, bytes.data());
            for (int i = 0; i < AsioPlaybackBuffer::sampleBytes(type); ++i) {
                check(bytes[i] == 0, "nonfinite audio becomes silence");
            }
        }
    }
    for (const float sample : {-1.0f, 0.25f, 1.0f}) {
        std::array<std::uint8_t, 8> bytes{};
        AsioPlaybackBuffer::encode(sample, 19, bytes.data());
        float single = 0;
        std::memcpy(&single, bytes.data(), 4);
        check(single == sample, "Float32 conversion");
        AsioPlaybackBuffer::encode(sample, 20, bytes.data());
        double wide = 0;
        std::memcpy(&wide, bytes.data(), 8);
        check(wide == sample, "Float64 conversion");
    }
    check(AsioPlaybackBuffer::sampleBytes(0) == 0, "unsupported MSB format is refused");
    AsioPlaybackBuffer ring(7);
    const std::array<float, 4> stereo{0.25f, -0.5f, 0.75f, -1};
    check(ring.read() == std::array<float,2>{0,0}, "underflow is stereo silence");
    for (int i = 0; i < 100; ++i) {
        check(ring.write(stereo) == 2, "write stereo frames across wrap");
        check(ring.read() == std::array<float,2>{0.25f,-0.5f}
              && ring.read() == std::array<float,2>{0.75f,-1}, "FIFO preserves channel order");
    }
    std::array<float, 20> full{};
    check(ring.write(full) == 7 && ring.write(stereo) == 0, "full FIFO never overwrites unread samples");
    ring.reset();
    check(ring.freeFrames() == 7 && ring.read() == std::array<float,2>{0,0}, "stopped reset discards old audio");
    std::thread consumer([&ring] {
        for (int i = 1; i <= 100000; ++i) {
            std::array<float,2> frame{};
            do { frame = ring.read(); } while (frame[0] == 0);
            check(frame == std::array<float,2>{static_cast<float>(i), -static_cast<float>(i)},
                  "concurrent callback reads each stereo frame exactly once");
        }
    });
    for (int i = 1; i <= 100000; ++i) {
        const std::array<float,2> frame{static_cast<float>(i), -static_cast<float>(i)};
        while (ring.write(frame) == 0) { std::this_thread::yield(); }
    }
    consumer.join();
    delayedRefills();
    prefillAndRecovery();
    std::cout << "ASIO output checks passed\n";
}
