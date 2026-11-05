#include "AnanClient.h"

#include <QVariantList>
#include <QVariantMap>

namespace AetherSDR::anan {
bool AnanClient::configurePureSignal(const PureSignalSettings& settings, PureSignalMode mode, std::shared_ptr<AnanTxDsp> prepared)
{
    if (!settings.valid() || m_transmitKeyed || !m_transmitDsp
        || !m_transmitConfigured || !prepared || prepared->mode() != m_transmitDsp->mode()
        || prepared->outputSamples() != m_transmitDsp->outputSamples()) { return false; }
    if (!transmitTransport([mode](auto* client) { return client->setPureSignalEnabled(mode != PureSignalMode::Off); })) { return false; }
    if (m_pureSignalMode == PureSignalMode::Off && mode != PureSignalMode::Off) {
        m_uncorrectedTransmitDsp = m_transmitDsp;
    }
    // A fresh channel is the OFF boundary: an old asynchronous correction can
    // never install into the channel that will produce the next transmission.
    clearTransmitAudio();
    m_transmitDsp = std::move(prepared);
    if (mode == PureSignalMode::Off) { m_uncorrectedTransmitDsp.reset(); }
    m_pureSignalMode = mode;
    m_pureSignalSettings = settings;
    m_pureSignalAttenuator = {};
    if (mode == PureSignalMode::Off) { m_feedbackAttenuationDb = 31; }
    return true;
}

void AnanClient::beginPureSignalOver()
{
    m_pureSignalFeedbackBlocks = 0;
    m_pureSignalFeedbackMs = 0;
    m_pureSignalTxPeak = m_pureSignalRxPeak = 0;
    if (!m_transmitDsp || !m_transmitDsp->pureSignalChannel()) { return; }
    const auto state = m_transmitDsp->pureSignalChannel()->pureSignalSnapshot(false);
    if (state) { m_pureSignalAttenuator.beginOver(state->information[5]); }
}

void AnanClient::updatePureSignalAttenuation()
{
    if ((m_pureSignalMode != PureSignalMode::Automatic && m_pureSignalMode != PureSignalMode::Single)
        || (!m_pureSignalSettings.autoAttenuate && !m_pureSignalAttenuator.pending())
        || !m_transmitDsp || !m_transmitKeyed || !m_transmitMediaStarted || m_transmitCarrier) { return; }
    const qint64 now = TxCoordinator::monotonicMs();
    if (now < m_nextAttenuationPollMs || !m_transmitOperation.permitsDispatch(now)
        || !m_transmitContext.permitsDispatch(now)) { return; }
    m_nextAttenuationPollMs = now + 100;
    WdspChannel* channel = m_transmitDsp->pureSignalChannel();
    if (!channel) { return; }
    const auto state = channel->pureSignalSnapshot(false);
    const auto paused = channel->pureSignalCalibrationPaused();
    if (!state || !paused) { return; }
    using Action = PureSignalAttenuator::Action;
    const Action action = m_pureSignalAttenuator.observe(*state,m_pureSignalSettings.autoAttenuate,
        *paused,m_feedbackAttenuationDb,now);
    bool accepted = false;
    if (action == Action::Pause) { accepted = channel->pausePureSignalCalibration(); }
    else if (action == Action::Attenuate) {
        const int db = m_pureSignalAttenuator.targetDb();
        accepted = transmitTransport([&](auto* transport) { return transport->setPureSignalAttenuation(db,m_transmitOperation); });
        if (!accepted) { failTransmit(QStringLiteral("Pure Signal feedback attenuation write refused")); return; }
        m_transmitDsp->resetPureSignalSpectrum();
        m_feedbackAttenuationDb = db;
    } else if (action == Action::Resume) { accepted = channel->resumePureSignalCalibration(m_pureSignalMode); }
    if (accepted) { m_pureSignalAttenuator.accepted(action,now,state->information[5]); }
}

QVariantMap AnanClient::pureSignalState(bool display, bool spectra)
{
    QVariantMap state{{QStringLiteral("autoAttenuate"),m_pureSignalSettings.autoAttenuate},
        {QStringLiteral("feedbackAttenuationDb"),m_feedbackAttenuationDb},
        {QStringLiteral("autoAttenuationPending"),m_pureSignalAttenuator.pending()},
        {QStringLiteral("mode"), static_cast<int>(m_pureSignalMode)},
        {QStringLiteral("moxWaitSeconds"), m_pureSignalSettings.moxWaitSeconds},
        {QStringLiteral("calibrationWaitSeconds"), m_pureSignalSettings.calibrationWaitSeconds},
        {QStringLiteral("amplifierDelayNs"), m_pureSignalSettings.amplifierDelayNs},
        {QStringLiteral("hardwarePeak"), m_pureSignalSettings.hardwarePeak},
        {QStringLiteral("keyed"),m_transmitKeyed},
        {QStringLiteral("routing"), m_pureSignalMode == PureSignalMode::Off
            ? QStringLiteral("PSCC inactive; feedback DDCs released.")
            : m_protocol == 1 ? QStringLiteral("PSCC: TX monitor slot 4; PA feedback slot 3; synchronized at 192000 Hz.")
                : QStringLiteral("PSCC: TX monitor DDC1; PA feedback DDC0; synchronized at 192000 Hz; RX on DDC2+." )}};
    const qint64 now = TxCoordinator::monotonicMs();
    const bool liveMedia = m_transmitKeyed && m_transmitMediaStarted && !m_transmitCarrier
        && m_transmitOperation.permitsDispatch(now) && m_transmitContext.permitsDispatch(now);
    const bool twoTone = liveMedia && m_transmitTwoTone;
    const bool freshFeedback = liveMedia && m_pureSignalFeedbackBlocks != 0
        && now >= m_pureSignalFeedbackMs && now-m_pureSignalFeedbackMs <= 500;
    state.insert(QStringLiteral("feedbackBlocks"),QVariant::fromValue(m_pureSignalFeedbackBlocks));
    state.insert(QStringLiteral("feedbackFresh"),freshFeedback);
    if (liveMedia && m_pureSignalMode != PureSignalMode::Off) {
        state.insert(QStringLiteral("feedbackStatus"),freshFeedback
            ? QStringLiteral("Receiving paired feedback: %1 blocks this transmission; latest TX peak %2, PA feedback peak %3 (digital full scale = 1).")
                .arg(m_pureSignalFeedbackBlocks).arg(m_pureSignalTxPeak,0,'g',5).arg(m_pureSignalRxPeak,0,'g',5)
            : QStringLiteral("Transmission active; no fresh paired feedback reaches calibration (%1 blocks this transmission).")
                .arg(m_pureSignalFeedbackBlocks));
    }
    state.insert(QStringLiteral("twoToneRunning"),twoTone);
    state.insert(QStringLiteral("spectraReady"),false);
    state.insert(QStringLiteral("imdValid"),false);
    state.insert(QStringLiteral("spectraStatus"),QStringLiteral("Prepare a TX session and arm Pure Signal for paired spectra."));
    if (!m_transmitDsp || !m_transmitDsp->pureSignalChannel()) { return state; }
    if (spectra) {
        m_transmitDsp->requestPureSignalSpectrum(true,now);
        const auto* data = liveMedia ? m_transmitDsp->pureSignalSpectrum(now) : nullptr;
        state.insert(QStringLiteral("spectraStatus"),m_pureSignalMode == PureSignalMode::Off
            ? QStringLiteral("Arm Pure Signal to acquire paired spectra; this does not start transmission.")
            : !m_transmitDsp->hasPureSignalSpectrum() ? QStringLiteral("Spectrum FFT initialization is unavailable; see the log.")
            : QStringLiteral("Waiting for fresh synchronized TX/PA feedback samples."));
        if (data) {
            const auto list = [](const std::vector<double>& values) {
                QVariantList result; result.reserve(static_cast<qsizetype>(values.size()));
                for (double value : values) { result.append(value); } return result;
            };
            state.insert(QStringLiteral("spectraReady"),true);
            state.insert(QStringLiteral("spectraFrequencyHz"),list(data->frequencyHz));
            state.insert(QStringLiteral("spectraTransmitDbfs"),list(data->transmitDbfs));
            state.insert(QStringLiteral("spectraFeedbackDbfs"),list(data->feedbackDbfs));
            state.insert(QStringLiteral("spectraClipping"),data->possibleClipping);
            state.insert(QStringLiteral("spectraAgeMs"),now-data->capturedAtMs);
            state.insert(QStringLiteral("spectraStatus"),data->possibleClipping
                ? QStringLiteral("Possible sample clipping: distortion measurements are unavailable.")
                : QStringLiteral("Paired TX reference and PA feedback, digital dBFS."));
            const bool imd = data->imdValid && twoTone && !m_pureSignalAttenuator.pending();
            state.insert(QStringLiteral("imdValid"),imd);
            if (imd) {
                QVariantList values; for (double value : data->imdDbc) { values.append(value); }
                state.insert(QStringLiteral("imdDbc"),values);
            }
        }
    }
    const auto snapshot = m_transmitDsp->pureSignalChannel()->pureSignalSnapshot(display);
    if (!snapshot) {
        state.insert(QStringLiteral("imdValid"),false); state.remove(QStringLiteral("imdDbc")); return state;
    }
    if (spectra && state.value(QStringLiteral("spectraReady")).toBool() && snapshot->information[4] > 256) {
        state.insert(QStringLiteral("imdValid"),false);
        state.remove(QStringLiteral("imdDbc"));
        state.insert(QStringLiteral("spectraClipping"),true);
        state.insert(QStringLiteral("spectraStatus"),QStringLiteral("Feedback exceeds the calibration peak; distortion measurements unavailable."));
    }
    QVariantList information;
    for (int value : snapshot->information) { information.append(value); }
    state.insert(QStringLiteral("information"), information);
    state.insert(QStringLiteral("maximumTransmit"), snapshot->maximumTransmit);
    state.insert(QStringLiteral("correcting"), snapshot->correctionRunning && !snapshot->correctionBusy);
    state.insert(QStringLiteral("correctionAvailable"), snapshot->correctionAvailable);
    state.insert(QStringLiteral("correctionRunning"), snapshot->correctionRunning);
    state.insert(QStringLiteral("correctionBusy"), snapshot->correctionBusy);
    if (display) {
        const auto series = [](const std::array<std::vector<double>, 4>& data) {
            QVariantList result;
            for (const std::vector<double>& values : data) {
                QVariantList points;
                for (double value : values) { points.append(value); }
                result.append(QVariant(points));
            }
            return result;
        };
        state.insert(QStringLiteral("measurements"), series(snapshot->measurements));
        state.insert(QStringLiteral("correction"), series(snapshot->correction));
        state.insert(QStringLiteral("phaseReferenceDegrees"), snapshot->phaseReferenceDegrees);
    }
    return state;
}
} // namespace AetherSDR::anan
