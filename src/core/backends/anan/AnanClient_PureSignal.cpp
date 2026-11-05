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
    return true;
}

QVariantMap AnanClient::pureSignalState(bool display)
{
    QVariantMap state{{QStringLiteral("mode"), static_cast<int>(m_pureSignalMode)},
        {QStringLiteral("moxWaitSeconds"), m_pureSignalSettings.moxWaitSeconds},
        {QStringLiteral("calibrationWaitSeconds"), m_pureSignalSettings.calibrationWaitSeconds},
        {QStringLiteral("amplifierDelayNs"), m_pureSignalSettings.amplifierDelayNs},
        {QStringLiteral("hardwarePeak"), m_pureSignalSettings.hardwarePeak},
        {QStringLiteral("keyed"),m_transmitKeyed},
        {QStringLiteral("routing"), m_pureSignalMode == PureSignalMode::Off
            ? QStringLiteral("PSCC inactive; feedback DDCs released.")
            : m_protocol == 1 ? QStringLiteral("PSCC: TX monitor slot 4; PA feedback slot 3; synchronized at 192000 Hz.")
                : QStringLiteral("PSCC: TX monitor DDC1; PA feedback DDC0; synchronized at 192000 Hz; RX on DDC2+." )}};
    if (!m_transmitDsp || !m_transmitDsp->pureSignalChannel()) { return state; }
    const auto snapshot = m_transmitDsp->pureSignalChannel()->pureSignalSnapshot(display);
    if (!snapshot) { return state; }
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
