#include "core/backends/anan/P1Client.h"
#include "core/backends/anan/AnanReceiveCalibration.h"
#include "core/backends/anan/AnanFilterOptions.h"
#include "AnanUdpReceiveBuffer.h"

#include <QNetworkDatagram>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcAnanP1, "aether.anan.p1", QtInfoMsg)
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR::anan {
std::optional<P1Client::Profile> P1Client::profile(int board, bool redPitaya) noexcept
{
    // Thetis accepts Red Pitaya reporting Hermes or Orion MkII.
    // Its explicit P1 model uses five slots, two ADCs and legacy HPF selection.
    if (redPitaya)
        return board == 1 || board == 5
            ? std::optional<Profile>{Profile{2, 5, false, ReceiveFilterKind::LegacyHpf}}
            : std::nullopt;
    // Thetis UpdateDDCs P1 layouts; RX1 is frame slot 0, not P2 DDC2.
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
    // Nereus BoardCapsTable::sampleRatesFor caps standard P1 boards
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
        // Distinguish stale/wrong P1 selection from a validated P1
        // radio that answered discovery but never started delivering IQ.
        fail(m_validated
            ? QStringLiteral("No validated OpenHPSDR P1 IQ arrived before the connection timeout.")
            : QStringLiteral("No valid OpenHPSDR P1 Discovery reply arrived before the connection timeout. Check the selected protocol and refresh discovery."));
    });
    m_countersTimer = new QTimer(this);
    m_countersTimer->setInterval(1000);
    connect(m_countersTimer, &QTimer::timeout, this, &P1Client::publishCounters);
}

P1Client::~P1Client() { stop(); }

bool P1Client::prepare(const Params& params)
{
    if (!validFrequencyFactor(params.frequencyCorrection) || !ananModelInfo(params.model)
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
    m_micAvailable = false;
    m_config.mic = {};
    m_receiveFilterPlan = {};
    m_params.redPitayaP1 = redPitaya;
    m_config = {};
    m_config.frequencyCorrection = params.frequencyCorrection;
    m_config.anvelinaExtraOc = params.model == AnanModel::AnvelinaPro3;
    m_config.rateKsps = params.ddc0RateKsps;
    m_config.antenna = params.rxAntenna;
    m_config.dither = params.ditherEnabled;
    m_config.random = params.randomEnabled;
    m_config.alexAttenuationDb = std::clamp(params.alexAttenuationDb / 10, 0, 3) * 10;
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
    const auto receiveBuffer = requestUdpReceiveBuffer(*m_socket);
    qCInfo(lcAnanP1).noquote() << QStringLiteral("ANAN P1: UDP receive buffer requested=%1 bytes; granted=%2")
        .arg(kUdpReceiveBufferBytes).arg(receiveBuffer ? QString::number(*receiveBuffer) + QStringLiteral(" bytes") : QStringLiteral("unavailable"));
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
    qCInfo(lcAnanP1) << "ANAN P1: discovery sent to" << m_host.toString() << "local UDP port" << m_socket->localPort();
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
    if (m_running && m_txControl) { enableTransmit(false); }
    clearTransmitAudio();
    m_transmitGate.transportClosed();
    m_txControl.reset();
    m_drainTimer->stop(); m_timeoutTimer->stop(); m_countersTimer->stop();
    if (m_running && m_validated) {
        if (m_config.rxOcMask != 0 || m_config.userDigitalOutputs != 0) {
            m_config.rxOcMask = 0;
            m_config.userDigitalOutputs = 0;
            const auto clearOc = p1::speakerAndControl(m_txSequence++, 0, 0, m_config);
            if (clearOc) { (void)send(*clearOc); }
            const auto clearDigital = p1::speakerAndControl(m_txSequence++, 0x14, 0x14, m_config);
            if (clearDigital) { (void)send(*clearDigital); }
        }
        (void)send(p1::streamCommand(false));
    }
    const bool hadSession = m_running;
    m_running = false; m_validated = false; m_linkUp = false;
    m_audio.clear(); m_expectedSequence.reset();
    if (m_socket) { m_socket->close(); m_socket->deleteLater(); m_socket = nullptr; }
    if (hadSession) {
        qCInfo(lcAnanP1) << "ANAN P1: session stopped; RX packets" << m_counters.rxPackets;
        publishCounters(); emit linkDown();
    }
}

void P1Client::fail(const QString& reason)
{
    qCWarning(lcAnanP1) << "ANAN P1: connection failed:" << reason;
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
        m_micAvailable = hasMicWiring(effectiveBoard) && m_params.model != AnanModel::RedPitaya;
        m_config.mic = m_micAvailable ? m_params.mic : MicOptions{};
        m_config.mic.xlr = false;
        m_config.receivers = m_profile->frameSlotCount;
        m_config.adcIndex = m_params.ddc0AdcIndex;
        updateFilters();
        m_addresses = {0, 0x1c, 0x12, 0x14, 0x16, 0x1e, 0x24};
        if (m_config.anvelinaExtraOc) { m_addresses.push_back(0x26); }
        for (int slot = 0; slot < m_profile->frameSlotCount; ++slot)
            m_addresses.push_back(static_cast<std::uint8_t>(4 + 2 * slot));
        m_validated = true;
        qCInfo(lcAnanP1) << "ANAN P1: validated board" << *boardId << "wire ID" << reply->boardId
            << "model" << QString::fromLatin1(descriptor ? descriptor->name.data() : "Unknown") << "override" << m_params.overrideDetection
            << "ADC" << m_config.adcIndex << "rate ksps" << m_config.rateKsps
            << "frame slots" << m_config.receivers;
        // Prime every RX control bank before enabling the IQ stream.
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
        for (int i = 0; i < m_config.receivers; ++i)
            emit ddcSequenceGap(i);
    }
    m_expectedSequence = frame->sequence + 1;
    ++m_counters.rxPackets;
    m_iq.clear();
    if (!p1::decodeIq(*frame, 0, m_iq))
        return;
    if (!m_linkUp) {
        qCInfo(lcAnanP1) << "ANAN P1: RX IQ started; slot 0, ADC" << m_config.adcIndex;
        m_linkUp = true; m_timeoutTimer->stop(); emit linkUp();
    }
    emit ddcIqReady(0, m_iq);
    emit ddc0IqReady(m_iq);
    for (int i = 1; i < m_config.receivers; ++i) {
        m_iq.clear();
        if (p1::decodeIq(*frame, i, m_iq))
            emit ddcIqReady(i, m_iq);
    }
}

void P1Client::updateFilters()
{
    if (!m_profile)
        return;
    // Reuse the verified frequency tables; translate relay positions
    // into P1 bank-local bits. P2 packet bytes never reach the P1 socket.
    const std::array<std::uint32_t, 1> words{phaseWord(m_config.frequencyHz)};
    const auto filters = m_params.filters.value_or(defaultFilterConfig(m_profile->filterKind == ReceiveFilterKind::MkIIBpf));
    const auto hp = buildHighPriority(true, words,
        m_params.bypassAdc0Filters, m_params.bypassAdc1Filters, 0, 0, 1, 0, true,
        m_profile->filterKind, m_profile->numAdcs,
        &filters, 0, &m_receiveFilterPlan);
    const std::uint32_t alex = (std::uint32_t{hp[1432]} << 24) | (std::uint32_t{hp[1433]} << 16)
        | (std::uint32_t{hp[1434]} << 8) | hp[1435];
    auto filter = [](std::uint32_t word) {
        std::uint8_t result = 0;
        constexpr std::array<int, 7> bits{1, 2, 4, 5, 6, 12, 3};
        for (std::size_t n = 0; n < bits.size(); ++n)
            if (word & (std::uint32_t{1} << bits[n])) result |= static_cast<std::uint8_t>(1u << n);
        return result;
    };
    m_config.rxOnlyInput = filters.rxOnlyInput;
    m_config.mkIIBpf = m_profile->filterKind == ReceiveFilterKind::MkIIBpf;
    if (m_config.mkIIBpf && m_config.alexAttenuationDb != 0) {
        m_config.adc0AttenuationDb = std::min(31, m_config.adc0AttenuationDb + m_config.alexAttenuationDb);
        m_config.alexAttenuationDb = 0;
    }
    m_config.rxOcMask = m_params.filters ? receiveOcMask(*m_params.filters, m_config.frequencyHz) : 0;
    m_config.userDigitalOutputs = filters.userDigitalOutputs;
    m_config.receiveFilterBits = filter(alex);
    // ADC0 band bypass/LNA controls must not alter ADC1's independent filter bits.
    m_config.adc1ReceiveFilterBits = filter((std::uint32_t{hp[1430]} << 8) | hp[1431]);
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
    const qint64 intentNow = TxCoordinator::monotonicMs();
    if (!m_transmitGate.poll(intentNow,[this](bool key) { return writeTransmitKey(key); })) {
        fail(QStringLiteral("P1 transmit stop failed")); return;
    }
    while (m_frameCredit >= p1::kSpeakerFrames) {
        if (m_transmitGate.keyCommandAccepted()) {
            std::array<std::complex<float>,p1::kSpeakerFrames> iq{};
            const bool media = !m_carrierEnabled && m_txCount >= iq.size();
            for (auto& sample : iq) {
                if (m_carrierEnabled) { sample = m_carrier.sample(48000); }
                else if (media) { sample = m_txIq[m_txHead]; m_txHead = (m_txHead + 1) % m_txIq.size(); --m_txCount; }
            }
            const auto first = nextAddress();
            const auto second = nextAddress();
            const auto packet = buildP1Transmit(m_config,*m_txControl,true,m_txSequence++,first,second,iq);
            const auto writer = [&] { return packet && send(*packet); };
            const bool accepted = media ? m_transmitGate.writeIq(m_txOperation,m_txContext,intentNow,writer)
                : m_transmitGate.writeCarrier(m_txOperation,intentNow,writer);
            if (!accepted) {
                enableTransmit(false);
                emit transmitFault(QStringLiteral("P1 TX packet or media authority refused"));
                return;
            }
            m_frameCredit -= p1::kSpeakerFrames;
            continue;
        }
        // DSP audio arrives in blocks, independently of EP2's clock.
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

void P1Client::clearTransmitAudio()
{
    m_txHead = 0; m_txCount = 0; m_txContext = {};
    m_carrierEnabled = false; m_carrier = {};
}

bool P1Client::configureTransmit(const P2TxControl& control)
{
    if (!m_running || !m_validated || !m_linkUp || m_transmitGate.enabled()
        || !validP2TxControl(control)) { return false; }
    m_txControl = control;
    const auto packet = buildP1Transmit(m_config,control,false,m_txSequence++,2,0x12);
    return packet && send(*packet);
}

bool P1Client::writeTransmitKey(bool key)
{
    if (!m_running || !m_validated || !m_txControl) { return false; }
    const auto packet = buildP1Transmit(m_config,*m_txControl,key,m_txSequence++,0,0x12);
    return packet && send(*packet);
}

bool P1Client::enableTransmit(bool enabled)
{
    if (enabled && (!m_txControl || !m_running || !m_validated || !m_linkUp)) { return false; }
    if (!enabled) { clearTransmitAudio(); }
    const bool ok = m_transmitGate.setEnabled(enabled,TxCoordinator::monotonicMs(),
        [this](bool key) { return writeTransmitKey(key); });
    if (!enabled) { m_txOperation = {}; }
    return ok;
}

bool P1Client::setTransmitKeying(bool key, const TxCoordinator::Operation& operation)
{
    if (!m_txControl || !m_running || !m_validated || !m_linkUp
        || (!key && !operation.sameAuthority(m_txOperation))) { return false; }
    if (!key || !m_transmitGate.keyCommandAccepted()) { clearTransmitAudio(); }
    const bool ok = m_transmitGate.setKeying(key,operation,TxCoordinator::monotonicMs(),
        [this](bool value) { return writeTransmitKey(value); });
    if (ok) {
        m_txOperation = key ? operation : TxCoordinator::Operation{};
        if (key) { m_audio.clear(); m_audioPrimed = false; }
    }
    return ok;
}

bool P1Client::updateTransmitDrive(int drive, const TxCoordinator::Operation& operation)
{
    if (!m_txControl || drive < 0 || drive > 255 || !operation.sameAuthority(m_txOperation)) { return false; }
    m_txControl->drive = drive;
    return m_transmitGate.writeCarrier(operation,TxCoordinator::monotonicMs(),[this] { return writeTransmitKey(true); });
}

bool P1Client::enqueueTransmitIq(std::span<const std::complex<float>> iq,
    const TxCoordinator::Operation& operation, const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (!transmitActive() || m_carrierEnabled || !operation.sameAuthority(m_txOperation)
        || !operation.permitsDispatch(now) || !context.permitsDispatch(now)
        || iq.size() > transmitQueueFreeSamples()) { return false; }
    if (m_txCount && !m_txContext.sameContext(context)) { return false; }
    for (const auto sample : iq) {
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag()) || std::abs(sample) > 1) { return false; }
    }
    m_txContext = context;
    for (const auto sample : iq) { m_txIq[(m_txHead + m_txCount++) % m_txIq.size()] = sample; }
    return true;
}

void P1Client::setCarrier(bool enabled, bool down)
{
    if (!transmitActive()) { return; }
    if (enabled && !m_carrierEnabled) { m_txCount = 0; m_txHead = 0; m_carrier = {}; }
    m_carrierEnabled = enabled; m_carrier.key(down);
}

void P1Client::setDdc0FrequencyHz(double hz)
{
    if (!std::isfinite(hz) || hz < 0 || hz > 61'440'000)
        return;
    m_config.frequencyHz = static_cast<std::uint32_t>(hz + 0.5);
    if (m_config.independentFrequencies)
        m_config.receiveFrequencies[0] = m_config.frequencyHz;
    updateFilters();
}
void P1Client::setFrequencyCorrection(double factor)
{
    if (!validFrequencyFactor(factor)) { return; }
    m_config.frequencyCorrection = factor;
    m_params.frequencyCorrection = factor;
}

void P1Client::setFrontEndAttenuation(int adc, int db, int alexDb)
{
    if (adc < 0 || adc >= m_config.numAdcs) { return; }
    if (adc == 0) {
        m_config.alexAttenuationDb = !m_config.mkIIBpf
            ? std::clamp(alexDb / 10, 0, 3) * 10 : 0;
    }
    setStepAttenuationDb(adc, db);
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

void P1Client::setHardwareFilters(const QVariantMap& options)
{
    setReceiveHardware(options, m_config.antenna);
}

void P1Client::setReceiveHardware(const QVariantMap& options, int antenna)
{
    const auto config = parseFilterOptions(options);
    if (!config || antenna < 1 || antenna > 3) { return; }
    m_config.antenna = antenna;
    m_params.filters = *config;
    m_params.bypassAdc0Filters = config->bypassAdc0;
    m_params.bypassAdc1Filters = config->bypassAdc1;
    updateFilters();
}

bool P1Client::setDdcRateLive(int ddc, int rate)
{
    if (!m_running || !m_validated || ddc != 0
        || (rate != 48 && rate != 96 && rate != 192 && rate != 384)
        || rate > (m_params.redPitayaP1 ? 384 : 192))
        return false;
    auto config = m_config;
    config.rateKsps = rate;
    const auto packet = p1::speakerAndControl(m_txSequence, 0, 0, config);
    if (!packet || !send(*packet)) { return false; }
    // A refused local write must not change subsequent periodic controls.
    ++m_txSequence;
    m_config = config;
    return true;
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
void P1Client::setMicOptions(const MicOptions& options)
{
    if (!m_validated || !m_micAvailable || options.xlr) { return; }
    m_config.mic = options;
    m_params.mic = options;
}
} // namespace AetherSDR::anan

namespace AetherSDR::anan {
bool P1Client::setReceiveStreams(const std::vector<DdcConfig>& streams,
                                 const std::vector<double>& centers)
{
    if (!m_running || !m_validated || streams.empty() || streams.size() != centers.size() ||
        streams.size() > std::size_t(std::min(4, m_config.receivers)))
        return false;
    for (std::size_t i = 0; i < streams.size(); ++i)
        if (streams[i].adcIndex != m_config.adcIndex || streams[i].rateKsps != m_config.rateKsps ||
            !std::isfinite(centers[i]) || centers[i] < 0 || centers[i] > 61'440'000)
            return false;
    m_config.independentFrequencies = true;
    m_config.frequencyHz = std::uint32_t(centers[0] + 0.5);
    m_config.receiveFrequencies.fill(m_config.frequencyHz);
    for (std::size_t i = 0; i < centers.size(); ++i)
        m_config.receiveFrequencies[i] = std::uint32_t(centers[i] + 0.5);
    updateFilters();
    return true;
}
} // namespace AetherSDR::anan
