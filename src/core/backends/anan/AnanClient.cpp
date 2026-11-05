#include "core/backends/anan/AnanClient.h"
#include <algorithm>
#include <complex>

namespace AetherSDR::anan {
AnanClient::AnanClient(QObject* parent) : QObject(parent),
    m_p1(new P1Client(this)), m_p2(new P2Client(this))
{
    // Ignore deliveries from the inactive transport. Selection is made
    // only after stopping the old session, on this object's own IO thread.
    auto wire = [this]<typename Client>(Client* client, int protocol) {
        connect(client, &Client::paCurrentSampled, this, [this, protocol](int counts) {
            if (m_protocol == protocol) { emit paCurrentSampled(counts); }
        });
        connect(client, &Client::supplyRailSampled, this, [this, protocol](int counts) {
            if (m_protocol == protocol) { emit supplyRailSampled(counts); }
        });
        connect(client, &Client::rfPowerSampled, this, [this, protocol](int forward, int reverse) {
            if (m_protocol == protocol) { emit rfPowerSampled(forward, reverse); }
        });
        connect(client, &Client::linkUp, this, [this, protocol] {
            if (m_protocol == protocol) emit linkUp();
        });
        connect(client, &Client::linkDown, this, [this, protocol] {
            if (m_protocol == protocol) emit linkDown();
        });
        connect(client, &Client::connectionError, this, [this, protocol](const QString& reason) {
            if (m_protocol == protocol) emit connectionError(reason);
        });
        connect(client, &Client::ddc0IqReady, this,
            [this, protocol](const std::vector<std::complex<float>>& block) {
                if (m_protocol == protocol) emit ddc0IqReady(block);
            }, Qt::DirectConnection);
        connect(
            client, &Client::ddcIqReady, this,
            [this, protocol](int index, const std::vector<std::complex<float>>& block) {
                if (m_protocol == protocol)
                    emit ddcIqReady(index, block);
            },
            Qt::DirectConnection);
        connect(client, &Client::ddcSequenceGap, this, [this, protocol](int index) {
            if (m_protocol == protocol) emit ddcSequenceGap(index);
        }, Qt::DirectConnection);
        connect(client, &Client::linkCountersUpdated, this,
            [this, protocol](const LinkCounters& counters) {
                if (m_protocol == protocol) emit linkCountersUpdated(counters);
            });
        connect(client, &Client::discoveryInfoReceived, this,
            [this, protocol](quint8 board, quint8 firmware, quint8 receivers) {
                if (m_protocol == protocol) {
                    // Thetis clsHardwareSpecific.PSDefaultPeak: protocol-shaped
                    // reference peaks; Saturn uses a distinct P2 DAC scale.
                    m_pureSignalSettings = {};
                    m_pureSignalSettings.hardwarePeak = protocol == 1 ? 0.4072 : board == 10 ? 0.6121 : 0.2899;
                    emit discoveryInfoReceived(board, firmware, receivers);
                }
            });
    };
    wire(m_p1, 1); wire(m_p2, 2);
    const auto feedback =
        [this](const std::vector<std::complex<float>>& tx, const std::vector<std::complex<float>>& rx) {
            if (m_pureSignalMode == PureSignalMode::Off || !m_transmitDsp
                || !m_transmitKeyed || !m_transmitMediaStarted || m_transmitCarrier
                || !m_transmitOperation.permitsDispatch(TxCoordinator::monotonicMs())
                || !m_transmitContext.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
            m_transmitDsp->feedPureSignalSpectrum(tx,rx,m_transmitTwoTone && !m_pureSignalAttenuator.pending(),
                TxCoordinator::monotonicMs());
            if (m_transmitDsp->feedPureSignalFeedback(tx,rx,TxCoordinator::monotonicMs())) {
                ++m_pureSignalFeedbackBlocks;
                m_pureSignalFeedbackMs = TxCoordinator::monotonicMs();
                m_pureSignalTxPeak = m_pureSignalRxPeak = 0;
                for (std::size_t i = 0; i < tx.size(); ++i) {
                    m_pureSignalTxPeak = std::max(m_pureSignalTxPeak,double(std::abs(tx[i])));
                    m_pureSignalRxPeak = std::max(m_pureSignalRxPeak,double(std::abs(rx[i])));
                }
                updatePureSignalAttenuation();
            }
        };
    const auto gap = [this] { if (m_transmitDsp) { m_transmitDsp->resetPureSignalSpectrum(); } };
    connect(m_p1,&P1Client::pureSignalFeedbackGap,this,gap,Qt::DirectConnection);
    connect(m_p2,&P2Client::pureSignalFeedbackGap,this,gap,Qt::DirectConnection);
    connect(m_p2, &P2Client::pureSignalFeedback, this, feedback, Qt::DirectConnection);
    connect(m_p1, &P1Client::pureSignalFeedback, this, feedback, Qt::DirectConnection);
    connect(m_p2, &P2Client::transmitFault, this, [this](const QString& reason) {
        clearTransmitAudio();
        m_transmitKeyed = false;
        m_transmitOperation = {};
        emit transmitFault(reason);
    });
    connect(m_p1, &P1Client::transmitFault, this, [this](const QString& reason) {
        if (m_protocol != 1) { return; }
        clearTransmitAudio(); m_transmitKeyed = false; m_transmitOperation = {};
        emit transmitFault(reason);
    });
}

bool AnanClient::start(const Params& params, int timeoutMs)
{
    stop();
    if (params.protocol != 1 && params.protocol != 2)
        return false;
    m_protocol = params.protocol;
    return m_protocol == 1 ? m_p1->start(params, timeoutMs) : m_p2->start(params, timeoutMs);
}
AnanClient::~AnanClient() { stop(); }
bool AnanClient::setDiversity(const DiversitySettings& settings)
{
    return transmitTransport([&settings](auto* client) { return client->setDiversity(settings); });
}
void AnanClient::stop()
{
    m_pureSignalMode = PureSignalMode::Off;
    m_pureSignalAttenuator = {};
    m_feedbackAttenuationDb = 31;
    m_transmitConfigured = false;
    if (m_protocol == 1) m_p1->stop(); else m_p2->stop();
    clearTransmitAudio();
    m_transmitDsp.reset();
    m_uncorrectedTransmitDsp.reset();
    m_transmitOperation = {};
    m_transmitKeyed = false;
}
void AnanClient::setDdc0FrequencyHz(double hz)
{
    if (m_protocol == 1) m_p1->setDdc0FrequencyHz(hz); else m_p2->setDdc0FrequencyHz(hz);
}
void AnanClient::setFrequencyCorrection(double factor)
{
    if (m_protocol == 1) { m_p1->setFrequencyCorrection(factor); }
    else { m_p2->setFrequencyCorrection(factor); }
}
void AnanClient::setMicOptions(const MicOptions& options)
{
    if (m_protocol == 1) { m_p1->setMicOptions(options); }
    else { m_p2->setMicOptions(options); }
}
void AnanClient::setFrontEndAttenuation(int adc, int db, int alexDb)
{
    if (m_protocol == 1) { m_p1->setFrontEndAttenuation(adc, db, alexDb); }
    else { m_p2->setFrontEndAttenuation(adc, db, alexDb); }
}
void AnanClient::setStepAttenuationDb(int adc, int db)
{
    if (m_protocol == 1) m_p1->setStepAttenuationDb(adc, db); else m_p2->setStepAttenuationDb(adc, db);
}
void AnanClient::setRxAntenna(int antenna)
{
    if (m_protocol == 1) m_p1->setRxAntenna(antenna); else m_p2->setRxAntenna(antenna);
}
void AnanClient::setReceiveHardware(const QVariantMap& options, int antenna)
{
    if (m_protocol == 1) { m_p1->setReceiveHardware(options, antenna); }
    else { m_p2->setReceiveHardware(options, antenna); }
}

void AnanClient::setHardwareFilters(const QVariantMap& options)
{
    if (m_protocol == 1) { m_p1->setHardwareFilters(options); }
    else { m_p2->setHardwareFilters(options); }
}
bool AnanClient::setDdcRateLive(int ddc, int rate)
{
    return m_protocol == 1 ? m_p1->setDdcRateLive(ddc, rate) : m_p2->setDdcRateLive(ddc, rate);
}
void AnanClient::enqueueSpeakerAudio(const QByteArray& audio)
{
    if (m_protocol == 1) m_p1->enqueueSpeakerAudio(audio); else m_p2->enqueueSpeakerAudio(audio);
}
} // namespace AetherSDR::anan

namespace AetherSDR::anan {
bool AnanClient::setReceiveStreams(const std::vector<DdcConfig>& streams,
                                   const std::vector<double>& centers)
{
    return m_protocol == 1 ? m_p1->setReceiveStreams(streams, centers)
                           : m_p2->setReceiveStreams(streams, centers);
}
} // namespace AetherSDR::anan

namespace AetherSDR::anan {
void AnanClient::setReceiveFilterPlan(const ReceiveFilterPlan& plan)
{
    if (m_protocol == 1) { m_p1->setReceiveFilterPlan(plan); }
    else { m_p2->setReceiveFilterPlan(plan); }
}
}

void AetherSDR::anan::AnanClient::setSpeakerAmpMuted(bool muted)
{
    if (m_protocol == 2) { m_p2->setSpeakerAmpMuted(muted); }
}
