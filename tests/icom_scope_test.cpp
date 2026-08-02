// IcomCIV Phase 2 — scope decoder test. Pins the WLAN single-packet path (the
// one the IC-705 backend actually runs), the centre-mode half-width span, the
// out-of-range short packet, the BCD division counters, and the USB
// multi-division assembly.
//
// Pure protocol: no sockets, no Qt, no hardware.

#include "core/backends/icom/IcomScope.h"

#include <cstdio>
#include <numeric>
#include <vector>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr std::uint8_t kIc705 = 0xA4;

// Build a 0x27 0x00 first-division frame.
static CivFrame makeFirst(int division, int divisionMax, ScopeMode mode, std::uint64_t freqA,
                          std::uint64_t freqB, bool outOfRange,
                          const std::vector<std::uint8_t>& waveform)
{
    std::vector<std::uint8_t> body;
    body.push_back(0x00);                                   // the fixed byte
    body.push_back(encodeBcdByte(division));
    body.push_back(encodeBcdByte(divisionMax));
    body.push_back(static_cast<std::uint8_t>(mode));
    const auto a = encodeFreq(freqA);
    const auto b = encodeFreq(freqB);
    body.insert(body.end(), a.begin(), a.end());
    body.insert(body.end(), b.begin(), b.end());
    body.push_back(outOfRange ? 0x01 : 0x00);
    body.insert(body.end(), waveform.begin(), waveform.end());

    auto wire = buildFrameSub(kIc705, cmd::kScope, scope::kWaveData, body);
    return *parseFrame(wire);
}

// Build a continuation division.
static CivFrame makeContinuation(int division, int divisionMax,
                                 const std::vector<std::uint8_t>& waveform)
{
    std::vector<std::uint8_t> body;
    body.push_back(0x00);
    body.push_back(encodeBcdByte(division));
    body.push_back(encodeBcdByte(divisionMax));
    body.insert(body.end(), waveform.begin(), waveform.end());
    auto wire = buildFrameSub(kIc705, cmd::kScope, scope::kWaveData, body);
    return *parseFrame(wire);
}

static void testWlanSinglePacket()
{
    // Over WLAN the division maximum is 1 and the WHOLE sweep arrives in one
    // packet. This is the path the IC-705 backend runs; the multi-division
    // machinery below never executes over WiFi.
    std::vector<std::uint8_t> wave(kScopePointsIc705);
    std::iota(wave.begin(), wave.end(), static_cast<std::uint8_t>(0));

    ScopeDecoder d;
    auto f = d.feed(makeFirst(1, 1, ScopeMode::Fixed, 14'000'000, 14'350'000, false, wave));
    check(f.has_value(), "a WLAN sweep completes in one frame");
    check(f->mode == ScopeMode::Fixed, "fixed mode");
    check(f->startHz == 14'000'000 && f->endHz == 14'350'000,
          "fixed mode reports the edges directly");
    check(f->raw.size() == static_cast<std::size_t>(kScopePointsIc705), "475 points");
    check(f->raw[0] == 0 && f->raw[10] == 10, "waveform data is positional and intact");
    check(!f->outOfRange, "in range");
}

static void testCentreModeHalfWidth()
{
    // THE factor-of-two. Icom's span is a HALF-width: the front panel reads
    // "±100k" and the display covers 200 kHz. Getting this backwards renders
    // signals at half or double their true offset from centre, which looks like
    // a tuning bug rather than a geometry bug.
    std::vector<std::uint8_t> wave(kScopePointsIc705, 80);

    ScopeDecoder d;
    auto f = d.feed(makeFirst(1, 1, ScopeMode::Centre, 14'100'000, 100'000, false, wave));
    check(f.has_value(), "centre-mode sweep decodes");
    check(f->startHz == 14'000'000, "start is centre MINUS span, not span/2");
    check(f->endHz == 14'200'000, "end is centre PLUS span");
    check(f->centreHz() == 14'100'000, "centre survives the round trip");
    check(f->bandwidthHz() == 200'000, "total bandwidth is TWICE the span");

    check(spanForBandwidthHz(200'000) == 100'000, "a 200 kHz request becomes a 100 kHz span");
    check(bandwidthForSpanHz(100'000) == 200'000, "and back again");
    auto widths = availableBandwidthsHz();
    check(widths.size() == kScopeSpansHz.size(), "eight reachable bandwidths");
    check(widths.front() == 5'000 && widths.back() == 1'000'000,
          "5 kHz to 1 MHz total — this is why Icom zoom SNAPS");
}

static void testOutOfRange()
{
    // The radio OMITS the waveform when out of range, so the packet is short.
    // We must still produce a frame: a waterfall that stops scrolling reads as
    // a hung application, while a flat one reads as "nothing here" — which is
    // what the radio is actually saying.
    ScopeDecoder d;
    auto f = d.feed(makeFirst(1, 1, ScopeMode::Fixed, 14'000'000, 14'350'000, true, {}));
    check(f.has_value(), "an out-of-range packet still produces a frame");
    check(f->outOfRange, "and is flagged");
    check(f->raw.size() == static_cast<std::size_t>(kScopePointsIc705),
          "floor-filled to full width so the waterfall keeps scrolling");
    check(f->raw[0] == 0 && f->raw[474] == 0, "at the floor");
}

static void testUsbMultiDivision()
{
    // Over USB the sweep is split into 11. Division counters are BCD, so
    // division 11 arrives as 0x11 — reading them as plain bytes works for 1..9
    // and then quietly breaks, which over WLAN never happens and so would ship.
    ScopeDecoder d;
    std::vector<std::uint8_t> chunk(50, 0x20);

    auto first = d.feed(makeFirst(1, 11, ScopeMode::Fixed, 14'000'000, 14'350'000, false, {}));
    check(!first.has_value(), "division 1 of 11 does not complete a sweep");

    for (int div = 2; div <= 10; ++div)
        check(!d.feed(makeContinuation(div, 11, chunk)).has_value(),
              "middle divisions accumulate");

    auto last = d.feed(makeContinuation(11, 11, chunk));
    check(last.has_value(), "division 11 completes the sweep");
    check(last->raw.size() == static_cast<std::size_t>(kScopePointsIc705),
          "and is padded/trimmed to the geometry");
    check(last->startHz == 14'000'000, "the header from division 1 survived");
}

static void testDivisionGapIsAbandoned()
{
    // The waveform is POSITIONAL and there is no per-division index inside the
    // payload to realign against, so a gap makes the sweep unrecoverable.
    // Concatenating anyway produces a shifted trace that looks plausible.
    ScopeDecoder d;
    (void)d.feed(makeFirst(1, 11, ScopeMode::Fixed, 14'000'000, 14'350'000, false, {}));
    (void)d.feed(makeContinuation(2, 11, std::vector<std::uint8_t>(50, 0x10)));
    auto skipped = d.feed(makeContinuation(4, 11, std::vector<std::uint8_t>(50, 0x10)));
    check(!skipped.has_value(), "a gap abandons the sweep");
    auto stillDead = d.feed(makeContinuation(11, 11, std::vector<std::uint8_t>(50, 0x10)));
    check(!stillDead.has_value(), "and it stays abandoned rather than emitting a shifted trace");

    // A fresh division 1 restarts cleanly.
    std::vector<std::uint8_t> wave(kScopePointsIc705, 40);
    auto recovered = d.feed(makeFirst(1, 1, ScopeMode::Fixed, 7'000'000, 7'200'000, false, wave));
    check(recovered.has_value(), "the next sweep recovers");
}

static void testNonScopeFramesIgnored()
{
    ScopeDecoder d;
    auto freq = parseFrame(cmdSetFrequency(kIc705, 14'195'000));
    check(!d.feed(*freq).has_value(), "a frequency frame is not scope data");
    auto other = parseFrame(cmdScopeOnOff(kIc705, true));
    check(!d.feed(*other).has_value(), "0x27 0x10 is not waveform data");
}

static void testDbmMapping()
{
    ScopeFrame f;
    f.raw = {0, 80, 160};
    ScopeGeometry g;
    ScopeCalibration cal;   // floor -140 dBm, span 80 dB, reference 0

    auto dbm = toDbm(f, g, cal);
    check(dbm.size() == 3, "one output per point");
    check(dbm[0] < -139.9f && dbm[0] > -140.1f, "display 0 maps to the floor");
    check(dbm[2] < -59.9f && dbm[2] > -60.1f, "display 160 maps to floor + span");
    check(dbm[1] < -99.9f && dbm[1] > -100.1f, "and it is linear in between");

    // The radio's own reference level shifts the whole trace. Read it back
    // rather than assuming zero — the operator can change it from the front
    // panel and we would otherwise track 20 dB out.
    cal.referenceDb = 10.0;
    auto shifted = toDbm(f, g, cal);
    check(shifted[0] < dbm[0], "a positive reference level shifts the trace down");

    // A byte above the published 0..160 range must clamp, not project above the
    // top of the scale and drag the waterfall's auto-contrast with it.
    ScopeFrame over;
    over.raw = {255};
    auto clamped = toDbm(over, g, ScopeCalibration{});
    check(clamped[0] < -59.9f && clamped[0] > -60.1f, "values above 160 clamp to the ceiling");
}

int main()
{
    testWlanSinglePacket();
    testCentreModeHalfWidth();
    testOutOfRange();
    testUsbMultiDivision();
    testDivisionGapIsAbandoned();
    testNonScopeFramesIgnored();
    testDbmMapping();

    if (g_failures == 0)
        std::printf("icom_scope_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
