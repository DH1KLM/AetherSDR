#include "core/backends/anan/P1Client.h"

#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR::anan {
std::optional<P1Client::Profile> P1Client::profile(int board, bool redPitaya) noexcept
{
    //DH1KLM: Thetis accepts Red Pitaya reporting Hermes or Orion MkII.
    // Its explicit P1 model uses five slots, two ADCs and legacy HPF selection.
    if (redPitaya)
        return board == 1 || board == 5
            ? std::optional<Profile>{Profile{2, 5, false, ReceiveFilterKind::LegacyHpf}}
            : std::nullopt;
    //DH1KLM: Thetis UpdateDDCs P1 layouts; RX1 is frame slot 0, not P2 DDC2.
    // Shared Orion MkII IDs still describe the standard physical board profile.
    switch (board) {
    case 1: return Profile{1, 4, true, ReceiveFilterKind::LegacyHpf};
    case 2: return Profile{1, 2, true, ReceiveFilterKind::LegacyHpf};
    case 3: case 4: return Profile{2, 5, false, ReceiveFilterKind::LegacyHpf};
    case 5: return Profile{2, 5, false, ReceiveFilterKind::MkIIBpf};
    case 20: return Profile{1, 4, false, ReceiveFilterKind::MkIIBpf};
    default: return std::nullopt; // HL2 stays with its existing backend.
    }
}

int P1Client::maximumRateKsps(int board, bool redPitaya) noexcept
{
    Q_UNUSED(board);
    //DH1KLM: Nereus BoardCapsTable::sampleRatesFor caps standard P1 boards
    // at 192 ksps. HL2 and explicit Red Pitaya model overrides are separate.
    return redPitaya ? 384 : 192;
}

P1Client::P1Client(QObject* parent) : QObject(parent)
{
    m_drainTimer = new QTimer(this);
    m_drainTimer->setInterval(5);
    m_drainTimer->setTimerType(Qt::PreciseTimer);
    connect(m_drainTimer, &QTimer::timeout, this, &P1Client::drain);
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("No validated OpenHPSDR P1 IQ arrived before the connection timeout."));
    });
    m_countersTimer = new QTimer(this);
    m_countersTimer->setInterval(1000);
    connect(m_countersTimer, &QTimer::timeout, this, &P1Client::publishCounters);
}

P1Client::~P1Client() { stop(); }

bool P1Client::prepare(const Params& params)
{
    if (!ananModelInfo(params.model)
        || (params.overrideDetection && params.model == AnanModel::Automatic))
        return false;
    const bool redPitaya = params.redPitayaP1 || params.model == AnanModel::RedPitaya;
    m_host = QHostAddress(params.host);
    if (m_host.protocol() != QAbstractSocket::IPv4Protocol || !params.activeDdcs.empty()
        || params.ddc0AdcIndex < 0 || params.ddc0AdcIndex > 1
        || (params.ddc0RateKsps != 48 && params.ddc0RateKsps != 96
            && params.ddc0RateKsps != 192 && params.ddc0RateKsps != 384)
        || params.rxAntenna < 1 || params.rxAntenna > 3
        || params.adc0AttenuationDb < 0 || params.adc0AttenuationDb > 31
        || params.adc1AttenuationDb < 0 || params.adc1AttenuationDb > 31)
        return false;
    if (params.expectedBoardId >= 0) {
        if (!receiveProfile(params.expectedBoardId, params.model, params.overrideDetection)) { return false; }
        const int effectiveBoard = params.model == AnanModel::Automatic
            ? params.expectedBoardId : ananModelInfo(params.model)->nativeBoardId;
        const auto selected = profile(effectiveBoard, redPitaya);
        if (!selected || params.ddc0AdcIndex >= selected->numAdcs
            || params.ddc0RateKsps > maximumRateKsps(params.expectedBoardId, redPitaya))
            return false;
    }
    m_params = params;
    m_params.redPitayaP1 = redPitaya;
    m_config = {};
    m_config.anvelinaExtraOc = params.model == AnanModel::AnvelinaPro3;
    m_config.rateKsps = params.ddc0RateKsps;
    m_config.antenna = params.rxAntenna;
    m_config.dither = params.ditherEnabled;
    m_config.random = params.randomEnabled;
    m_config.adc0AttenuationDb = params.adc0AttenuationDb;
    m_config.adc1AttenuationDb = params.adc1AttenuationDb;
    m_profile.reset(); m_expectedSequence.reset();
    m_addresses.clear(); m_addressIndex = 0;
    m_audio.clear(); m_iq.clear(); m_txSequence = 0;
    m_counters = {}; m_frameCredit = 0; m_lastClockNs = 0;
    m_validated = false; m_linkUp = false; m_running = true;
    m_audioPrimed = false;
    return true;
}

bool P1Client::start(const Params& params, int timeoutMs)
{
    stop();
    if (!prepare(params))
        return false;
    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        stop(); return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, [this] {
        while (m_socket && m_socket->hasPendingDatagrams()) {
            const auto datagram = m_socket->receiveDatagram();
            const auto bytes = datagram.data();
            handleDatagram(datagram.senderAddress(), datagram.senderPort(),
                {reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                 static_cast<std::size_t>(bytes.size())});
        }
    });
    const auto request = p1::discoveryRequest();
    if (!send(request)) { stop(); return false; }
    m_timeoutTimer->start(std::max(1, timeoutMs));
    m_countersTimer->start();
    return true;
}

bool P1Client::send(std::span<const std::uint8_t> bytes)
{
    bool accepted = false;
    if (m_sendOverride)
        accepted = m_sendOverride(bytes);
    else if (m_socket)
        accepted = m_socket->writeDatagram(reinterpret_cast<const char*>(bytes.data()),
            static_cast<qint64>(bytes.size()), m_host, kRadioPort) == static_cast<qint64>(bytes.size());
    if (accepted)
        m_counters.txBytes += bytes.size();
    return accepted;
}

void P1Client::stop()
{
    m_drainTimer->stop(); m_timeoutTimer->stop(); m_countersTimer->stop();
    if (m_running && m_validated)
        (void)send(p1::streamCommand(false));
    const bool hadSession = m_running;
    m_running = false; m_validated = false; m_linkUp = false;
    m_audio.clear(); m_expectedSequence.reset();
    if (m_socket) { m_socket->close(); m_socket->deleteLater(); m_socket = nullptr; }
    if (hadSession) { publishCounters(); emit linkDown(); }
}

void P1Client::fail(const QString& reason)
{
    stop(); emit connectionError(reason);
}

void P1Client::handleDatagram(const QHostAddress& sender, quint16 port,
                            std::span<const std::uint8_t> bytes)
{
    if (!m_running || sender != m_host || port != kRadioPort)
        return;
    m_counters.rxBytes += bytes.size();
    if (!m_validated) {
        const auto reply = p1::parseDiscovery(bytes);
        if (!reply)
            return;
        const auto boardId = p1::normalizedBoardId(reply->boardId);
        const auto* descriptor = ananModelInfo(m_params.model);
        const int effectiveBoard = m_params.model == AnanModel::Automatic
            ? boardId.value_or(-1) : (descriptor ? descriptor->nativeBoardId : -1);
        m_profile = boardId ? profile(effectiveBoard, m_params.redPitayaP1) : std::nullopt;
        if (!m_profile || !receiveProfile(boardId.value_or(-1), m_params.model, m_params.overrideDetection)
            || (m_params.expectedBoardId >= 0 && m_params.expectedBoardId != *boardId)
            || m_params.ddc0AdcIndex >= m_profile->numAdcs
            || m_params.ddc0RateKsps > maximumRateKsps(boardId.value_or(-1), m_params.redPitayaP1)) {
            fail(QStringLiteral("The P1 discovery board/profile does not match this receive session."));
            return;
        }
        if (reply->streaming) {
            fail(QStringLiteral("The OpenHPSDR P1 radio is already in use."));
            return;
        }
        m_config.numAdcs = m_profile->numAdcs;
        m_config.receivers = m_profile->frameSlotCount;
        m_config.adcIndex = m_params.ddc0AdcIndex;
        updateFilters();
        m_addresses = {0, 0x1c, 0x12, 0x14, 0x16, 0x1e, 0x24};
        if (m_config.anvelinaExtraOc) { m_addresses.push_back(0x26); }
        for (int slot = 0; slot < m_profile->frameSlotCount; ++slot)
            m_addresses.push_back(static_cast<std::uint8_t>(4 + 2 * slot));
        m_validated = true;
        //DH1KLM: Prime every RX control bank before enabling the IQ stream.
        for (std::size_t n = 0; n < (m_addresses.size() + 1) / 2; ++n) {
            const auto first = nextAddress();
            const auto second = nextAddress();
            const auto packet = p1::speakerAndControl(m_txSequence++, first, second, m_config);
            if (!packet || !send(*packet)) { fail(QStringLiteral("Could not prime P1 receive controls.")); return; }
        }
        if (!send(p1::streamCommand(true))) { fail(QStringLiteral("Could not start P1 receive.")); return; }
        emit discoveryInfoReceived(static_cast<quint8>(*boardId), reply->firmware, m_profile->frameSlotCount);
        m_clock.start(); m_lastClockNs = 0; m_frameCredit = 0;
        m_drainTimer->start();
        return;
    }
    const auto frame = p1::parseIq(bytes, m_config.receivers);
    if (!frame)
        return;
    if (m_expectedSequence && frame->sequence != *m_expectedSequence) {
        const auto gap = frame->sequence - *m_expectedSequence;
        // Modular forward gaps count losses; duplicate/rewound frames are discarded.
        if (gap >= 0x80000000u)
            return;
        m_counters.drops += gap;
        emit ddcSequenceGap(0);
    }
    m_expectedSequence = frame->sequence + 1;
    ++m_counters.rxPackets;
    m_iq.clear();
    if (!p1::decodeIq(*frame, 0, m_iq))
        return;
    if (!m_linkUp) {
        m_linkUp = true; m_timeoutTimer->stop(); emit linkUp();
    }
    emit ddc0IqReady(m_iq);
}

void P1Client::updateFilters()
{
    if (!m_profile)
        return;
    //DH1KLM: Reuse the verified frequency tables; translate relay positions
    // into P1 bank-local bits. P2 packet bytes never reach the P1 socket.
    const std::array<std::uint32_t, 1> words{phaseWord(m_config.frequencyHz)};
    const auto hp = buildHighPriority(true, words,
        m_params.bypassAdc0Filters, m_params.bypassAdc1Filters, 0, 0, 1, 0, true,
        m_profile->filterKind, m_profile->numAdcs);
    const std::uint32_t alex = (std::uint32_t{hp[1432]} << 24) | (std::uint32_t{hp[1433]} << 16)
        | (std::uint32_t{hp[1434]} << 8) | hp[1435];
    auto filter = [](std::uint32_t word) {
        std::uint8_t result = 0;
        constexpr std::array<int, 7> bits{1, 2, 4, 5, 6, 12, 3};
        for (std::size_t n = 0; n < bits.size(); ++n)
            if (word & (std::uint32_t{1} << bits[n])) result |= static_cast<std::uint8_t>(1u << n);
        return result;
    };
    m_config.receiveFilterBits = filter(alex);
    const auto adc1Hp = buildHighPriority(true, words, m_params.bypassAdc1Filters,
        true, 0, 0, 1, 0, false, m_profile->filterKind, m_profile->numAdcs);
    m_config.adc1ReceiveFilterBits = filter((std::uint32_t{adc1Hp[1432]} << 24)
        | (std::uint32_t{adc1Hp[1433]} << 16) | (std::uint32_t{adc1Hp[1434]} << 8) | adc1Hp[1435]);
    constexpr std::array<int, 7> lpfBits{20, 21, 22, 23, 29, 30, 31};
    m_config.lowPassBits = 0;
    for (std::size_t n = 0; n < lpfBits.size(); ++n)
        if (alex & (std::uint32_t{1} << lpfBits[n]))
            m_config.lowPassBits |= static_cast<std::uint8_t>(1u << n);
}

std::uint8_t P1Client::nextAddress()
{
    const auto address = m_addresses[m_addressIndex];
    m_addressIndex = (m_addressIndex + 1) % m_addresses.size();
    return address;
}

void P1Client::drain()
{
    drainAt(m_clock.nsecsElapsed());
}

void P1Client::drainAt(qint64 now)
{
    if (!m_running || !m_validated)
        return;
    m_frameCredit = std::min(4.0 * p1::kSpeakerFrames,
        m_frameCredit + (now - m_lastClockNs) * (48000.0 / 1e9));
    m_lastClockNs = now;
    while (m_frameCredit >= p1::kSpeakerFrames) {
        //DH1KLM: DSP audio arrives in blocks, independently of EP2's clock.
        // Prime two blocks, then consume only complete packets; padding each
        // short tail would insert periodic gaps between otherwise continuous audio.
        if (!m_audioPrimed && m_audio.size() >= 4096)
            m_audioPrimed = true;
        if (m_audioPrimed && m_audio.size() < 2 * p1::kSpeakerFrames)
            m_audioPrimed = false;
        const std::size_t count = m_audioPrimed ? 2 * p1::kSpeakerFrames : 0;
        const auto first = nextAddress();
        const auto second = nextAddress();
        const auto packet = p1::speakerAndControl(m_txSequence++, first, second,
            m_config, std::span<const std::int16_t>(m_audio).first(count), m_profile->swapSpeaker);
        if (!packet || !send(*packet)) {
            fail(QStringLiteral("Could not send P1 receive audio/control.")); return;
        }
        m_audio.erase(m_audio.begin(), m_audio.begin() + count);
        m_frameCredit -= p1::kSpeakerFrames;
    }
}

void P1Client::setDdc0FrequencyHz(double hz)
{
    if (!std::isfinite(hz) || hz < 0 || hz > 61'440'000)
        return;
    m_config.frequencyHz = static_cast<std::uint32_t>(hz + 0.5);
    updateFilters();
}

void P1Client::setStepAttenuationDb(int adc, int db)
{
    if (adc == 0) m_config.adc0AttenuationDb = std::clamp(db, 0, 31);
    else if (adc == 1 && m_config.numAdcs == 2) m_config.adc1AttenuationDb = std::clamp(db, 0, 31);
}

void P1Client::setRxAntenna(int antenna)
{
    if (antenna >= 1 && antenna <= 3) m_config.antenna = antenna;
}

bool P1Client::setDdcRateLive(int ddc, int rate)
{
    if (!m_running || !m_validated || ddc != 0
        || (rate != 48 && rate != 96 && rate != 192 && rate != 384)
        || rate > (m_params.redPitayaP1 ? 384 : 192))
        return false;
    m_config.rateKsps = rate;
    const auto packet = p1::speakerAndControl(m_txSequence++, 0, 0, m_config);
    return packet && send(*packet);
}

void P1Client::enqueueSpeakerAudio(const QByteArray& audio)
{
    if (!m_running || !m_validated || !m_params.speakerAudioEnabled || (audio.size() % 4) != 0)
        return;
    constexpr std::size_t maximumSamples = 8192;
    const auto incoming = static_cast<std::size_t>(audio.size() / 2);
    const auto keep = std::min(incoming, maximumSamples);
    if (m_audio.size() + keep > maximumSamples)
        m_audio.erase(m_audio.begin(), m_audio.begin() + (m_audio.size() + keep - maximumSamples));
    const auto oldSize = m_audio.size();
    m_audio.resize(oldSize + keep);
    std::memcpy(m_audio.data() + oldSize, audio.constData() + (incoming - keep) * 2, keep * 2);
}

void P1Client::publishCounters()
{
    if (m_socket)
        m_counters.localEndpoint = QStringLiteral("%1:%2").arg(m_socket->localAddress().toString()).arg(m_socket->localPort());
    emit linkCountersUpdated(m_counters);
}
} // namespace AetherSDR::anan
