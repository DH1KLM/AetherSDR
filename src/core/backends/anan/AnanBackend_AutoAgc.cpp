#include "AnanBackend.h"
#include <QVariantList>
#include <cmath>
#include <algorithm>

namespace AetherSDR::anan {
ReceiveDispatch AnanBackend::requestSliceAgcThresholdMode(int id, AgcThresholdMode mode)
{
    if (!m_connected || !m_receiveLayout.slice(id)
        || (mode != AgcThresholdMode::Manual && mode != AgcThresholdMode::Automatic)) {
        return ReceiveDispatch::Unsupported;
    }
    const bool enabled = mode == AgcThresholdMode::Automatic;
    if (id == 0) {
        if (m_autoAgcEnabled != enabled) {
            if (enabled) { m_manualAgcBeforeAuto = m_agcThreshold; }
            m_autoAgcEnabled = enabled;
            m_primaryAutoAgcRevision = ++m_autoAgcRevision;
            pushAutoAgc(0);
        }
        emitSliceState();
    } else {
        const auto it = m_additionalRx.find(id);
        if (it == m_additionalRx.end() || !it->second->ready) { return ReceiveDispatch::Unsupported; }
        if (it->second->autoAgc != enabled) {
            it->second->autoAgc = enabled;
            it->second->autoAgcRevision = ++m_autoAgcRevision;
            pushAutoAgc(id);
        }
        publishAdditionalRx(id);
    }
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::pushAutoAgc(int id)
{
    QPointer<AnanRxDsp> dsp = m_dsp;
    bool enabled = m_autoAgcEnabled;
    double offset = m_autoAgcOffset;
    quint64 revision = m_primaryAutoAgcRevision;
    if (id != 0) {
        const auto it = m_additionalRx.find(id);
        if (it == m_additionalRx.end() || !it->second->ready) { return; }
        dsp = it->second->dsp;
        enabled = it->second->autoAgc;
        offset = it->second->autoAgcOffset;
        revision = it->second->autoAgcRevision;
    }
    if (dsp) {
        QMetaObject::invokeMethod(dsp, [dsp, enabled, offset, revision] {
            if (dsp) { dsp->setAutoAgc(enabled, offset, revision); }
        }, Qt::QueuedConnection);
    }
}

void AnanBackend::onAutoAgcApplied(int id, quint64 revision, double topDb)
{
    if (!m_connected || !m_receiveLayout.slice(id) || !std::isfinite(topDb)
        || topDb < -20.0 || topDb > 120.0) { return; }
    // Existing AGC-T presentation maps 0..100 to the WDSP ceiling -20..120 dB.
    const int knob = std::clamp(static_cast<int>(std::lround((topDb + 20.0) / 1.4)), 0, 100);
    if (id == 0) {
        if (!m_autoAgcEnabled || revision != m_primaryAutoAgcRevision) { return; }
        m_agcThreshold = knob;
        emitSliceState();
    } else {
        const auto it = m_additionalRx.find(id);
        if (it == m_additionalRx.end() || !it->second->autoAgc || revision != it->second->autoAgcRevision) { return; }
        it->second->threshold = knob;
        publishAdditionalRx(id);
    }
}

bool AnanBackend::setAutoAgcOffset(int id, double offsetDb)
{
    if (!m_connected || !m_receiveLayout.slice(id) || !std::isfinite(offsetDb)
        || offsetDb < -60.0 || offsetDb > 60.0) { return false; }
    if (id == 0) {
        if (m_autoAgcOffset == offsetDb) { return true; }
        m_autoAgcOffset = offsetDb;
        m_primaryAutoAgcRevision = ++m_autoAgcRevision;
    } else {
        const auto it = m_additionalRx.find(id);
        if (it == m_additionalRx.end() || !it->second->ready) { return false; }
        if (it->second->autoAgcOffset == offsetDb) { return true; }
        it->second->autoAgcOffset = offsetDb;
        it->second->autoAgcRevision = ++m_autoAgcRevision;
    }
    pushAutoAgc(id);
    return true;
}

QVariantMap AnanBackend::autoAgcState() const
{
    QVariantList slices;
    for (const auto& [id, slice] : m_receiveLayout.slices()) {
        Q_UNUSED(slice);
        bool enabled = m_autoAgcEnabled;
        double offset = m_autoAgcOffset;
        if (id != 0) {
            const auto it = m_additionalRx.find(id);
            if (it == m_additionalRx.end() || !it->second->ready) { continue; }
            enabled = it->second->autoAgc;
            offset = it->second->autoAgcOffset;
        }
        slices.append(QVariantMap{{QStringLiteral("slice"), id}, {QStringLiteral("enabled"), enabled},
                                 {QStringLiteral("offsetDb"), offset}});
    }
    return {{QStringLiteral("identity"), QStringLiteral("%1:%2").arg(m_radioSerial).arg(m_connectGeneration)},
            {QStringLiteral("slices"), slices}};
}
}
