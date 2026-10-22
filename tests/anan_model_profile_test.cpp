//DH1KLM: Source-pinned logical models, independent of sockets and GUI labels.
#include "core/backends/anan/P2Protocol.h"
#include "core/backends/anan/AnanReceiveCalibration.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace AetherSDR::anan;
static std::uint32_t readU32be(const std::uint8_t* bytes)
{
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16)
        | (std::uint32_t{bytes[2]} << 8) | bytes[3];
}
static void check(bool value, const char* claim)
{
    if (!value) { std::cerr << "FAIL: " << claim << '\n'; std::exit(1); }
}
//DH1KLM: ANT1/2/3 use P2 ALEX bits 24..26 and P1 bank-0 C4 bits 0..1.
// Switching antennas must preserve the rest of the receive configuration.
static void antennaIsolation(const ReceiveProfile& profile, bool supportsP1)
{
    for (const int hz : {3'800'000, 14'200'000, 51'000'000}) {
        for (const bool bypass : {false, true}) {
            auto filters = defaultFilterConfig(profile.filterKind == ReceiveFilterKind::MkIIBpf);
            filters.ocEnabled = true;
            filters.ocReceive[0] = {0, 61'440'000, 0x55};
            const std::array<std::uint32_t, 1> words{phaseWord(hz)};
            const auto highPriority = [&](int antenna) {
                return buildHighPriority(true, words, bypass, bypass, 17, 23, antenna,
                    profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind,
                    profile.numAdcs, &filters);
            };
            const auto baseline = highPriority(1);
            for (int antenna = 1; antenna <= 3; ++antenna) {
                auto packet = highPriority(antenna);
                check((packet[1432] & 7u) == (1u << (antenna - 1)), "P2 exact ANT1/2/3 relay bit");
                check((packet[1428] & 7u) == (profile.selectReceiveLpf ? (1u << (antenna - 1)) : 0u),
                      "P2 mirrored antenna only on profiles using the shared LPF word");
                check(packet[4] == 1, "antenna selection keeps P2 in RX without PTT");
                packet[1432] = (packet[1432] & ~7u) | (baseline[1432] & 7u);
                packet[1428] = (packet[1428] & ~7u) | (baseline[1428] & 7u);
                check(packet == baseline, "P2 antenna leaves tuning, filters, OC and attenuation unchanged");
            }
            if (!supportsP1) { continue; }
            p1::ReceiveConfig config;
            config.numAdcs = profile.numAdcs;
            config.adcIndex = profile.numAdcs - 1;
            config.frequencyHz = hz;
            config.rxOcMask = 0x55;
            config.receiveFilterBits = bypass ? 0x20 : 0x02;
            config.adc1ReceiveFilterBits = bypass ? 0x20 : 0x04;
            config.adc0AttenuationDb = 17;
            config.adc1AttenuationDb = 23;
            for (int address = 0; address <= 0x26; ++address) {
                config.antenna = 1;
                const auto original = p1::receiveControl(address, config);
                for (int antenna = 1; antenna <= 3; ++antenna) {
                    config.antenna = antenna;
                    auto control = p1::receiveControl(address, config);
                    check(control.has_value() == original.has_value(), "P1 antenna cannot authorize another control bank");
                    if (!control) { continue; }
                    check(((*control)[0] & 1) == 0, "P1 antenna keeps MOX clear");
                    if (address == 0) {
                        check(((*control)[4] & 3) == antenna - 1, "P1 exact ANT1/2/3 selector");
                        (*control)[4] = (*original)[4];
                    }
                    check(control == original, "P1 antenna leaves ADC routing, frequency, filters and OC unchanged");
                }
            }
        }
    }
}
//DH1KLM: Thetis network.h bits 8..11/14 and networkproto1.c bank-0 C3.
static void receiveInputIsolation(const ReceiveProfile& profile, bool supportsP1)
{
    const bool mkII = profile.filterKind == ReceiveFilterKind::MkIIBpf;
    const std::array<unsigned, 4> alexBits = mkII
        ? std::array<unsigned, 4>{0, 0x0c00, 0x4200, 0x4100}
        : std::array<unsigned, 4>{0, 0x0c00, 0x0a00, 0x0900};
    const std::array<unsigned, 4> c3Bits = mkII
        ? std::array<unsigned, 4>{0, 0xa0, 0x41, 0x61}
        : std::array<unsigned, 4>{0, 0xa0, 0xc0, 0xe0};
    auto filters = defaultFilterConfig(mkII);
    filters.ocEnabled = true;
    filters.ocReceive[0] = {0,61'440'000,0x55};
    const std::array<std::uint32_t, 1> words{phaseWord(14'200'000)};
    const auto packet = [&] {
        return buildHighPriority(true, words, false, false, 17, 23, 2,
            profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind,
            profile.numAdcs, &filters);
    };
    const auto original = packet();
    p1::ReceiveConfig config;
    config.numAdcs = profile.numAdcs;
    config.adcIndex = profile.numAdcs - 1;
    config.antenna = 2;
    config.dither = config.random = true;
    config.rxOcMask = 0x55;
    config.mkIIBpf = mkII;
    const auto general = p1::receiveControl(0, config);
    for (int input = 0; input <= 3; ++input) {
        filters.rxOnlyInput = input;
        auto actual = packet();
        check((actual[1434] & 0x4fu) == (alexBits[input] >> 8), "P2 RX input and RX_OUT/master exact bits");
        actual[1434] &= ~0x4fu;
        check(actual == original, "P2 input preserves ANT relays, ADC1, filters, OC, tuning and PTT");
        if (!supportsP1) { continue; }
        config.rxOnlyInput = input;
        auto control = p1::receiveControl(0, config);
        check(control && ((*control)[3] & 0xe1u) == c3Bits[input], "P1 RX input and RX_OUT/master exact C3 bits");
        (*control)[3] &= ~0xe1u;
        check(control == general, "P1 input preserves dither, random, OC and antenna bank");
        for (int address = 1; address <= 0x26; ++address) {
            const auto changed = p1::receiveControl(address, config);
            config.rxOnlyInput = 0;
            check(changed == p1::receiveControl(address, config), "P1 input changes only the general control bank");
            config.rxOnlyInput = input;
        }
    }
    filters.rxOnlyInput = 0;
    check(packet() == original, "return to ANT bank clears dedicated-input relays");
    config.rxOnlyInput = 4;
    check(!p1::receiveControl(0, config), "invalid P1 receive input refused");
}
//DH1KLM: Independent ADC1 boundaries must survive ADC0 bypass and input routing.
static void adc1FilterIsolation(const ReceiveProfile& profile)
{
    auto filters = defaultFilterConfig(profile.filterKind == ReceiveFilterKind::MkIIBpf);
    filters.receiveAdc1 = std::array<FilterBand, 6>{{
        {0,999999}, {1000000,2999999}, {3000000,7999999},
        {8000000,15999999}, {16000000,31999999}, {32000000,61440000}}};
    filters.receiveBypass.fill(true);
    filters.disable6mLnaOnRx = true;
    constexpr std::array<unsigned, 6> bits{6,5,4,1,2,3};
    for (std::size_t row = 0; row < bits.size(); ++row) {
        const auto band = (*filters.receiveAdc1)[row];
        for (const auto hz : {band.lowHz, (band.lowHz + band.highHz) / 2, band.highHz}) {
            const std::array<std::uint32_t, 1> words{phaseWord(hz)};
            for (const bool bypass : {false, true}) {
                for (int input = 0; input <= 3; ++input) {
                    filters.rxOnlyInput = input;
                    const auto packet = buildHighPriority(true, words, true, bypass, 17, 23, 2,
                        profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind,
                        profile.numAdcs, &filters);
                    const unsigned lower = (unsigned{packet[1430]} << 8) | packet[1431];
                    const unsigned expected = profile.numAdcs == 1 ? 0
                        : bypass ? 0x1000 : 1u << bits[row];
                    check(lower == expected, "P2 ADC1 exact band/bypass bits at independent range edges");
                    check((packet[1434] & 0x10) != 0 && packet[1443] == 17
                          && packet[1442] == (profile.numAdcs == 2 ? 23 : 0),
                          "ADC1 filtering preserves ADC0 bypass and independent attenuation");
                }
            }
        }
    }
    for (int adc = 0; adc < profile.numAdcs; ++adc) {
        const std::array<DdcConfig, 1> ddcs{{{96, adc}}};
        const auto packet = buildDdcSpecific(ddcs, profile.numAdcs, true, true, profile.firstDdcIndex);
        check(packet[7] == (1u << profile.firstDdcIndex)
              && packet[17 + 6 * profile.firstDdcIndex] == adc,
              "ADC source selection retains the model's physical RX DDC");
    }
    check(hasAlex2Filters(profile)
              == (profile.numAdcs == 2 && profile.filterKind == ReceiveFilterKind::MkIIBpf),
          "Alex-2 availability follows the effective filter hardware, not the physical board alone");
}
static void mainAntennaChoices(const ReceiveProfile& profile, AnanModel model, int board)
{
    const auto names = receiveInputNames(model, board);
    for (int antenna = 1; antenna <= 3; ++antenna) {
        const auto bank = receiveAntennaSelection(profile.numAdcs, 0, 0, antenna, names);
        check(bank.count == 3 && bank.ports == std::array<std::string_view,3>{"ANT1","ANT2","ANT3"}
              && bank.current == bank.ports[antenna-1], "main RX antenna publishes all three working ANT ports");
        for (int input = 1; input <= 3; ++input) {
            const auto external = receiveAntennaSelection(profile.numAdcs, 0, input, antenna, names);
            check(external.count == 1 && external.current == names[input] && external.ports[0] == names[input],
                  "dedicated ADC0 input never pretends that ANT bank controls its active source");
        }
        for (int input = 0; input <= 3; ++input) {
            const auto adc1 = receiveAntennaSelection(profile.numAdcs, 1, input, antenna, names);
            check(profile.numAdcs == 1 ? adc1.count == 0
                      : adc1.count == 1 && adc1.current == "RX2" && adc1.ports[0] == "RX2",
                  "main RX antenna reflects fixed ADC1 and refuses absent ADC1");
        }
    }
    check(receiveAntennaSelection(profile.numAdcs, -1, 0, 1, names).count == 0
          && receiveAntennaSelection(profile.numAdcs, 0, 4, 1, names).count == 0
          && receiveAntennaSelection(profile.numAdcs, 0, 0, 4, names).count == 0,
          "invalid antenna route cannot publish selectable ports");
}
static void frontEndAttenuation()
{
    for (const auto& entry : kAnanModels) {
        if (entry.model == AnanModel::Automatic) { continue; }
        const bool expected = entry.model == AnanModel::Hermes || entry.model == AnanModel::Anan100
            || entry.model == AnanModel::Anan100B || entry.model == AnanModel::Anan100D
            || entry.model == AnanModel::Anan200D;
        for (int physical : {1, 2, 3, 4, 5, 10, 20}) {
            check(hasClassicAlexAttenuator(entry.model, physical) == expected,
                  "logical analogue profile governs ATT even on a different digital board");
        }
    }
    for (const bool classic : {false, true}) {
        for (int db = -1; db <= 65; ++db) {
            const auto att = receiveAttenuation(db, classic);
            const int total = std::clamp(db, 0, classic ? 61 : 31);
            check(att.totalDb == total && att.adcDb + att.alexDb == total,
                  "total attenuation and bounded stages agree");
            check(att.adcDb >= 0 && att.adcDb <= 31 && att.alexDb == (classic && total > 31 ? 30 : 0),
                  "continuous ATT uses ALEX 30 only above ADC's 31 dB limit");
        }
        for (int step = 0; step <= (classic ? 5 : 3); ++step) {
            const auto att = receiveAttenuationPreset(step, classic);
            check(att.totalDb == step * 10 && att.adcDb + att.alexDb == att.totalDb,
                  "Thetis preset total");
            check(att.adcDb == (!classic ? step * 10 : step <= 3 ? 0 : 20),
                  "Thetis presets use the exact ADC stage");
            const std::array<std::uint32_t, 1> words{phaseWord(14'200'000)};
            const auto packet = buildHighPriority(true, words, false, true, att.adcDb, 19, 2, 0,
                true, classic ? ReceiveFilterKind::LegacyHpf : ReceiveFilterKind::MkIIBpf, 2, nullptr, att.alexDb);
            const int stages = att.alexDb / 10;
            check((packet[1434] & 0x60) == ((stages & 1 ? 0x40 : 0) | (stages & 2 ? 0x20 : 0)),
                  "P2 exact classic ALEX 10/20 dB relay bits");
            check(packet[1443] == att.adcDb && packet[1442] == 19 && packet[4] == 1,
                  "P2 preset preserves sibling ADC and never keys");
            p1::ReceiveConfig config;
            config.mkIIBpf = !classic; config.numAdcs = 2;
            config.adc0AttenuationDb = att.adcDb; config.adc1AttenuationDb = 19;
            config.alexAttenuationDb = att.alexDb;
            check(((*p1::receiveControl(0, config))[3] & 3) == stages,
                  "P1 bank zero exact ALEX attenuator bits");
            check((*p1::receiveControl(0x14, config))[4] == (0x20 | att.adcDb),
                  "P1 exact ADC attenuator bits");
        }
    }
    const std::array<std::uint32_t, 1> words{phaseWord(14'200'000)};
    for (int input = 0; input <= 3; ++input) {
        auto filters = defaultFilterConfig(true); filters.rxOnlyInput = input;
        const auto baseline = buildHighPriority(true, words, false, true, 7, 9, 3, 2, true,
            ReceiveFilterKind::MkIIBpf, 2, &filters);
        check(buildHighPriority(true, words, false, true, 7, 9, 3, 2, true,
            ReceiveFilterKind::MkIIBpf, 2, &filters, 30) == baseline,
            "MkII ignores classic ATT without changing the reused input relay");
    }
    p1::ReceiveConfig invalid; invalid.mkIIBpf = true; invalid.alexAttenuationDb = 30;
    check(!p1::receiveControl(0, invalid), "P1 refuses classic ATT on repurposed MkII bits");
}

static void bandViews()
{
    for (const int maximum : {192, 384, 1536}) {
        const auto view = receiveBandView(14'000'000, 14'350'000, 14'200'000, maximum, 100, 2900, 0);
        check(view && view->rateKsps == (maximum < 768 ? maximum : 768),
              "20m chooses a ceiling rate including display margin, within the session limit");
        // 384 * .90 = 345.6 kHz, so 350 kHz needs the next P2 rate.
        check(view && view->wholeBand == (maximum >= 384),
              "whole-band flag reflects the usable capture, including native RP 384");
        const auto tuning = receiveTuning(true, view->centerHz, 14'200'000,
            view->rateKsps * 1000.0, 100, 2900, 0);
        check(tuning && tuning->centerHz == view->centerHz,
              "band view retains the receive passband without CTUN recentering");
    }
    const auto twenty = receiveBandView(14e6, 14.35e6, 14.225e6, 1536, 100, 2900, 0);
    check(twenty && std::abs(twenty->spanHz * 0.92 - 357'000) < 0.001
          && twenty->spanHz < twenty->rateKsps * 1000,
          "20m display fits band edges with 1% margins instead of the full 768k capture");
    std::vector<float> ramp(4096);
    for (std::size_t i = 0; i < ramp.size(); ++i) { ramp[i] = static_cast<float>(i); }
    const double fraction = twenty->spanHz / (twenty->rateKsps * 1000.0);
    const auto cropped = bandViewSpectrum(ramp, fraction);
    for (const int bin : {0, 512, 2048, 3584, 4095}) {
        const double expected = 2048 + (bin - 2048) * fraction;
        check(std::abs(cropped[bin] - expected) < 0.001,
              "cropped spectrum coordinates match the published display span on both sides of DC");
    }
    check(bandViewSpectrum(ramp, 1) == ramp && bandViewSpectrum({}, 0.5).empty(),
          "full-span and empty spectra retain their original data");
    const auto wideFilter = receiveBandView(10'100'000, 10'150'000, 10'125'000,
        1536, -100'000, 100'000, 0);
    check(wideFilter && wideFilter->rateKsps == 384 && wideFilter->wholeBand,
          "rate selection includes a passband wider than the regional band");
    check(!receiveBandView(10'100'000, 10'150'000, 10'125'000, 192, -100'000, 100'000, 0),
          "refuse a filter wider than every supported usable capture");
    const auto narrow = receiveBandView(5'351'500, 5'366'500, 5'357'000, 1536, 100, 2900, 0);
    check(narrow && narrow->rateKsps == 48 && narrow->wholeBand,
          "regional 60m envelope fits 48 ksps");
    const auto eighty = receiveBandView(3'500'000, 3'800'000, 3'700'000, 384, -2900, -100, 0);
    check(eighty && eighty->rateKsps == 384 && eighty->wholeBand,
          "regional 80m fits native Red Pitaya P1 384 ksps");
    for (const double rx : {50'000'000.0, 53'999'000.0}) {
        for (const double bfo : {-600.0, 0.0, 600.0}) {
            const auto wide = receiveBandView(50'000'000, 54'000'000, rx, 1536, -2900, 2900, bfo);
            check(wide && wide->rateKsps == 1536 && !wide->wholeBand,
                  "6m uses maximum span and declares partial coverage");
            const auto tuning = receiveTuning(true, wide->centerHz, rx, wide->rateKsps * 1000.0,
                -2900, 2900, bfo);
            check(tuning && std::abs(tuning->centerHz - wide->centerHz) < 0.001,
                  "wide-band edge clamping preserves RX, full filter and CW shift");
        }
    }
    check(!receiveBandView(14e6, 14.35e6, 7.1e6, 1536, 100, 2900, 0)
          && !receiveBandView(14e6, 14e6, 14e6, 1536, 100, 2900, 0)
          && !receiveBandView(14e6, 14.35e6, 14.2e6, 47, 100, 2900, 0)
          && !receiveBandView(14e6, INFINITY, 14.2e6, 1536, 100, 2900, 0)
          && !receiveBandView(14e6, 14.35e6, NAN, 1536, 100, 2900, 0),
          "band view refuses missing allocation, invalid bounds, rate and nonfinite input");
}

static void ctunTuning()
{
    constexpr double center = 14'200'000;
    for (const int rate : {48'000, 96'000, 192'000, 384'000, 768'000, 1'536'000}) {
        for (const double bfo : {-600.0, 0.0, 600.0}) {
            for (const double offset : {-1000.0, 0.0, 1000.0}) {
                const auto on = receiveTuning(true, center, center + offset, rate, -200, 2900, bfo);
                check(on && on->centerHz == center && on->shiftHz == offset - bfo,
                      "CTUN holds NCO, uses the raw-wire DSP shift sign and preserves CW BFO");
                const auto off = receiveTuning(false, center, center + offset, rate, -200, 2900, bfo);
                check(off && off->centerHz == center + offset && off->shiftHz == -bfo,
                      "CTUN off follows the dial with only CW shift");
            }
            const double edge = rate * 0.46 - 2900 - bfo;
            const auto fits = receiveTuning(true, center, center + edge, rate, -200, 2900, bfo);
            check(fits && fits->centerHz == center, "filter touching usable edge retains CTUN anchor");
            const double lowerEdge = -rate * 0.46 - std::min(0.0, -200 + bfo);
            const auto lowerFits = receiveTuning(true, center, center + lowerEdge, rate, -200, 2900, bfo);
            const auto lowerOutside = receiveTuning(true, center, center + lowerEdge - 1, rate, -200, 2900, bfo);
            check(lowerFits && lowerFits->centerHz == center && lowerOutside
                  && lowerOutside->centerHz == center + lowerEdge - 1,
                  "negative CTUN edge includes the lower filter and CW margin");
            const auto outside = receiveTuning(true, center, center + edge + 1, rate, -200, 2900, bfo);
            check(outside && outside->centerHz == center + edge + 1 && outside->shiftHz == -bfo,
                  "crossing filter edge recenters instead of silently receiving aliased IQ");
        }
    }
    const auto wide = receiveTuning(true, center, center + 100'000, 1'536'000, 100, 2900, 0);
    const auto narrow = receiveTuning(true, wide->centerHz, center + 100'000, 48'000, 100, 2900, 0);
    check(wide->centerHz == center && narrow->centerHz == center + 100'000,
          "narrower landed rate recenters an offset which no longer fits");
    const auto dragged = receiveTuning(true, center + 5000, center, 48000, 100, 2900, 0);
    check(dragged && dragged->centerHz == center + 5000 && dragged->shiftHz == -5000,
          "explicit spectrum drag preserves receiver frequency and adjusts its DSP offset");
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), -1.0, 61'440'001.0}) {
        check(!receiveTuning(true, center, invalid, 48000, 100, 2900, 0),
              "invalid tuning intent is refused before DSP or NCO mutation");
    }
    check(!receiveTuning(true, center, center, 0, 100, 2900, 0)
          && !receiveTuning(true, center, center, 48000, 2900, 100, 0),
          "invalid sample geometry and reversed filters are refused");
}

//DH1KLM Independent SWL relay state, HF priority and RX-only wire encoding.
static void swlRouting()
{
    auto filters = defaultFilterConfig(false);
    filters.ocSwl[4] = {5060000,7199999,0x41};
    filters.ocSwl[6] = {9000000,9989999,0x22};
    filters.ocReceive[3] = {7000000,7300000,0};
    check(receiveOcMask(filters,6100000) == 0, "SWL defaults off behind shared switch");
    filters.ocEnabled = true;
    check(receiveOcMask(filters,5060000) == 0x41 && receiveOcMask(filters,6999999) == 0x41,
          "SWL range endpoints select their own mask");
    check(receiveOcMask(filters,7100000) == 0, "unassigned HF range suppresses overlapping SWL");
    filters.ocReceive[3].mask = 4;
    check(receiveOcMask(filters,7100000) == 4, "HF masks replace rather than combine SWL");
    check(receiveOcMask(filters,9500000) == 0x22 && receiveOcMask(filters,12000000) == 0,
          "SWL band changes clear previous assignments and gaps clear all pins");
    check(receiveOcMask(filters,std::numeric_limits<double>::quiet_NaN()) == 0,
          "invalid frequency cannot assert SWL pins");
    const std::array<std::uint32_t,1> words{phaseWord(6100000)};
    const auto packet = buildHighPriority(true, words, false, false, 0, 0, 1,
        0, true, ReceiveFilterKind::LegacyHpf, 1, &filters);
    check(packet[1401] == 0x82 && (packet[4] & 0x1e) == 0, "P2 SWL pins occupy only RX OC bits");
    const auto stopped = buildHighPriority(false, words, false, false, 0, 0, 1,
        0, true, ReceiveFilterKind::LegacyHpf, 1, &filters);
    check(stopped[1401] == 0, "P2 stop clears SWL pins");
    p1::ReceiveConfig config;
    config.rxOcMask = receiveOcMask(filters,6100000);
    const auto control = p1::receiveControl(0,config);
    check(control && (*control)[2] == 0x82 && ((*control)[0] & 1) == 0,
          "P1 SWL pins preserve RX-only control bits");
}
int main()
{
    //DH1KLM Known positive/negative carrier offsets follow Thetis' correction direction.
    check(std::abs(*calibratedFrequencyFactor(1.0, 10000000, 100)-1.00001) < 1e-12,
          "positive reference offset increases oscillator tuning");
    check(!calibratedFrequencyFactor(1.0, 0, 100)
          && !calibratedFrequencyFactor(1.0, 100000, 2500), "invalid or excessive calibration refused");
    std::vector<float> reference(1024, -120);
    reference[513] = -60; reference[514] = -40; reference[515] = -60;
    const auto peak = calibrationPeak(reference, 48000);
    check(peak && std::abs(peak->offsetHz-93.75) < 0.001 && peak->snrDb >= 20,
          "reference peak uses the raw FFT centre and correct sign");
    reference[530] = -41;
    check(!calibrationPeak(reference,48000), "ambiguous reference carrier refused");
    //DH1KLM Exercise every supported native model's P1/P2 tuning without changing its relays.
    for (const auto& entry : kAnanModels) {
        if (entry.model == AnanModel::Automatic || entry.model == AnanModel::Hpsdr
            || entry.model == AnanModel::HermesLite2) { continue; }
        const auto profile = receiveProfile(entry.nativeBoardId, entry.model);
        check(profile.has_value(), "every enabled logical model has a native receive profile");
        const std::array<std::uint32_t,2> words{phaseWord(10000000),phaseWord(14000000)};
        const auto normal = buildHighPriority(true, words, false, true, 0, 0, 1,
            profile->firstDdcIndex, true, profile->filterKind, profile->numAdcs);
        const auto corrected = buildHighPriority(true, words, false, true, 0, 0, 1,
            profile->firstDdcIndex, true, profile->filterKind, profile->numAdcs, nullptr, 0, nullptr, 1.00001);
        const std::size_t offset = 9+4*profile->firstDdcIndex;
        check(readU32be(corrected.data()+offset) == static_cast<std::uint32_t>(double(words[0])*1.00001)
              && readU32be(corrected.data()+offset+4) == static_cast<std::uint32_t>(double(words[1])*1.00001),
              "model-specific physical P2 DDC rows receive the frequency correction");
        check(std::equal(normal.begin()+1400,normal.end(),corrected.begin()+1400)
              && (corrected[4] & 0x1e) == 0, "frequency correction preserves model relays and TX refusal");
        p1::ReceiveConfig config;
        config.frequencyHz = 10000000; config.frequencyCorrection = 1.00001;
        const auto control = p1::receiveControl(0x04, config);
        check(control && readU32be(control->data()+1) == 10000100, "P1 encodes calibrated integer Hz");
    }
    //DH1KLM Thetis user nibble is independent of OC and never carries keying bits.
    for (int mask = 0; mask <= 15; ++mask) {
        FilterConfig filters = defaultFilterConfig(false);
        filters.userDigitalOutputs = static_cast<std::uint8_t>(mask);
        const std::array<std::uint32_t, 1> words{phaseWord(6100000)};
        const auto packet = buildHighPriority(true, words, false, false, 0, 0, 1,
            0, true, ReceiveFilterKind::LegacyHpf, 1, &filters);
        const auto stopped = buildHighPriority(false, words, false, false, 0, 0, 1,
            0, true, ReceiveFilterKind::LegacyHpf, 1, &filters);
        check(packet[1402] == mask && packet[1401] == 0 && (packet[4] & 0x1e) == 0,
              "P2 user bits are independent of OC and TX");
        check(stopped[1402] == 0, "P2 stop clears manual user outputs");
        p1::ReceiveConfig config;
        config.userDigitalOutputs = static_cast<std::uint8_t>(mask);
        const auto bank = p1::receiveControl(0x14, config);
        check(bank && (*bank)[0] == 0x14 && (*bank)[1] == 0x40
              && (*bank)[2] == 0 && (*bank)[3] == mask && (*bank)[4] == 0x20,
              "P1 manual user outputs preserve disabled mic PTT and ADC attenuation");
        config.userDigitalOutputs = 16;
        check(!p1::receiveControl(0x14, config), "P1 rejects bits beyond the user nibble");
    }
    swlRouting();
    ctunTuning();
    bandViews();
    frontEndAttenuation();
    check(kAnanModels.size() == 18, "all 17 Thetis logical IDs plus automatic");
    for (int id = -1; id <= 16; ++id) {
        const auto model = static_cast<AnanModel>(id);
        const auto* info = ananModelInfo(model);
        check(info && info->model == model && !info->name.empty(), "model IDs have unique names");
        if (id < 0 || model == AnanModel::Hpsdr || model == AnanModel::HermesLite2) { continue; }
        const auto profile = receiveProfile(info->nativeBoardId, model);
        check(profile.has_value(), "all ANAN logical models resolve P2 RX");
        check(profile->numAdcs == (id <= 5 || model == AnanModel::AnanG2E ? 1 : 2), "model ADC counts");
        check(profile->firstDdcIndex == (info->nativeBoardId == 10 || profile->numAdcs == 1 ? 0 : 2),
              "preserve Saturn bench DDC0 and select correct physical DDC elsewhere");
        const auto expectedFilter = id <= 7 || model == AnanModel::RedPitaya
            ? ReceiveFilterKind::LegacyHpf : ReceiveFilterKind::MkIIBpf;
        check(profile->filterKind == expectedFilter, "model-specific legacy HPF or MkII BPF");
        check(maximumP1RateKsps(model) == (model == AnanModel::RedPitaya ? 384 : 192), "P1 model rate ceiling");
        for (const int wrong : {0, 6, 11, 99}) {
            check(!receiveProfile(wrong, model), "unrelated/unqualified wire ID cannot inherit model profile");
        }
    }
    //DH1KLM: The explicit override may cross every implemented ANAN board
    // family; its effective layout must equal the selected native profile.
    for (const int board : {1, 2, 3, 4, 5, 10, 20}) {
        for (const auto& entry : kAnanModels) {
            if (entry.model == AnanModel::Automatic || !receiveProfile(entry.nativeBoardId)) { continue; }
            const auto actual = receiveProfile(board, entry.model, true);
            const auto wanted = receiveProfile(entry.nativeBoardId, entry.model);
            if (actual) {
                antennaIsolation(*actual, entry.nativeBoardId != 10);
                receiveInputIsolation(*actual, entry.nativeBoardId != 10);
                adc1FilterIsolation(*actual);
                mainAntennaChoices(*actual, entry.model, board);
            }
            check(actual && wanted && actual->numAdcs == wanted->numAdcs
                  && actual->firstDdcIndex == wanted->firstDdcIndex
                  && actual->filterKind == wanted->filterKind
                  && actual->physicalDdcLimit == wanted->physicalDdcLimit
                  && actual->swapSpeakerChannels == wanted->swapSpeakerChannels
                  && actual->saturnDroopDefaults == wanted->saturnDroopDefaults,
                  "board override uses the complete explicit model profile");
        }
    }
    check(!receiveProfile(5, AnanModel::Automatic, true), "override requires explicit model");
    check(!receiveProfile(5, static_cast<AnanModel>(99), true), "override rejects unknown model");
    for (const int board : {0, 6, 11, 99}) {
        check(!receiveProfile(board, AnanModel::Anan200D, true), "override cannot authorize unsupported physical board");
    }
    check(!receiveProfile(5, AnanModel::HermesLite2, true), "override cannot switch to HL2 P2");
    check(!ananModelInfo(static_cast<AnanModel>(17)), "LAST sentinel is not a model");
    check(!receiveProfile(5, static_cast<AnanModel>(99)), "unknown model rejected");
    check(!receiveProfile(6, AnanModel::HermesLite2), "HL2 P2 is excluded");
    check(!receiveProfile(11, AnanModel::AnanG2), "planned Saturn MkII cannot silently inherit G2");
    check(receiveProfile(1, AnanModel::Anan10E)->physicalDdcLimit == 2,
          "Thetis-compatible Hermes ID still uses selected Hermes II model layout");
    for (const int board : {1, 5}) {
        const auto rp = receiveProfile(board, AnanModel::RedPitaya);
        check(rp && rp->numAdcs == 2 && rp->firstDdcIndex == 2
            && rp->filterKind == ReceiveFilterKind::LegacyHpf, "Red Pitaya P2 overrides shared discovery identity");
    }
    check(receiveInputNames(AnanModel::Hermes,1)[1] == "RX1"
          && receiveInputNames(AnanModel::Anan100,1)[1] == "EXT2"
          && receiveInputNames(AnanModel::Anan200D,5)[1] == "EXT2"
          && receiveInputNames(AnanModel::RedPitaya,1)[1] == "BYPASS"
          && receiveInputNames(AnanModel::Anan7000,5)[1] == "BYPASS",
          "connector labels follow the mapped model rather than physical board");
    check(receiveInputNames(AnanModel::Automatic,1)[1] == "RX1"
          && receiveInputNames(AnanModel::Automatic,4)[1] == "EXT2"
          && receiveInputNames(AnanModel::Automatic,5)[1] == "BYPASS",
          "automatic connector labels follow the board family");
    p1::ReceiveConfig config;
    check(!p1::receiveControl(0x26, config), "Anvelina OC bank excluded by default");
    config.anvelinaExtraOc = true;
    check(p1::receiveControl(0x26, config) == p1::Control{0x26, 0, 0, 0, 0}, "Anvelina RX extra OC stays zero");
    std::cout << "ANAN model profiles passed\n";
}
