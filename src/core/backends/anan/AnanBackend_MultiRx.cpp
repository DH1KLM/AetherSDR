#include "core/backends/WdspNoiseBlanker.h"
#include "core/backends/anan/AnanBackend.h"
#include <QMetaObject>
#include <cmath>

namespace AetherSDR::anan {
namespace {
// Uninstalled channels also own WDSP/FFTW resources. Release stale or failed
// builds on the planner, never on the GUI or UDP thread.
template <class T> void releasePlanned(QObject* planner, std::shared_ptr<T> value)
{
    QMetaObject::invokeMethod(
        planner, [value = std::move(value)]() mutable { value.reset(); }, Qt::QueuedConnection);
}
QString rxPan(int n)
{
    return QStringLiteral("anan-%1").arg(n);
}
QByteArray rxBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()), int(v.size() * sizeof(float))};
}
} // namespace
int AnanBackend::receiveSliceCeiling() const
{
    const auto p = receiveProfile(m_discoveryInfoReceived ? m_discoveredBoardId
                                                          : m_pendingParams.expectedBoardId,
                                  m_pendingParams.model, m_pendingParams.overrideDetection);
    return p && p->numAdcs == 1 && p->filterKind == ReceiveFilterKind::LegacyHpf
               ? std::min(4, p->physicalDdcLimit)
               : 5;
}
int AnanBackend::receiveStreamCeiling() const
{
    const int board =
        m_discoveryInfoReceived ? m_discoveredBoardId : m_pendingParams.expectedBoardId;
    const auto physical = receiveProfile(board),
               effective =
                   receiveProfile(board, m_pendingParams.model, m_pendingParams.overrideDetection);
    if (!physical || !effective)
        return 1;
    if (m_pendingParams.protocol == 1) {
        const auto p = P1Client::profile(effectiveBoardId(), m_pendingParams.redPitayaP1);
        return p ? std::min(4, p->frameSlotCount) : 1;
    }
    int n = std::min({5, physical->physicalDdcLimit - effective->firstDdcIndex,
                      effective->physicalDdcLimit - effective->firstDdcIndex});
    if (m_discoveredReceiverCount > 0)
        n = std::min(n, int(m_discoveredReceiverCount));
    return std::max(1, n);
}
double AnanBackend::frontEndReferenceHz() const
{
    if (m_connected)
        for (const auto& w : m_receiveLayout.streams())
            if (w.active)
                return w.centerHz;
    return m_ddcCenterHz;
}
bool AnanBackend::primaryVisible() const
{
    return m_receiveLayout.slice(0) != nullptr;
}
int AnanBackend::primaryPanIndex() const
{
    const auto* s = m_receiveLayout.slice(0);
    return s ? s->stream : 0;
}
QString AnanBackend::primaryPanId() const
{
    return rxPan(primaryPanIndex());
}
int AnanBackend::streamForPan(const QString& pan) const
{
    for (int st = 0; st < m_receiveLayout.limit(); ++st)
        if (m_receiveLayout.streams()[st].active && rxPan(st) == pan)
            return st;
    return -1;
}
void AnanBackend::retireEmptyReceiveWindows(const std::array<AnanReceiveLayout::Stream, 5>& before)
{
    for (int st = 0; st < 5; ++st)
        if (before[st].active && !m_receiveLayout.streams()[st].active)
            emit panRemoved(rxPan(st));
}
void AnanBackend::publishReceiveWindows()
{
    for (int st = 0; st < m_receiveLayout.limit(); ++st) {
        const auto& w = m_receiveLayout.streams()[st];
        if (!w.active)
            continue;
        if (primaryVisible() && st == primaryPanIndex()) {
            emitPanState();
            continue;
        }
        emit panBandwidthLimitsChanged(rxPan(st), 0.048,
                                       nearestSessionRateKsps(kDdc0RatesKsps.back()) / 1000.0);
        emit panCenterBandwidthChanged(rxPan(st), w.centerHz / 1e6, w.rateKsps / 1000.0);
    }
}
std::vector<ReceiveFilterDemand> AnanBackend::receiveFilterDemands() const
{
    std::vector<ReceiveFilterDemand> demands;
    for (const auto& [id, slice] : m_receiveLayout.slices()) {
        if (id != 0) {
            const auto it = m_additionalRx.find(id);
            if (it == m_additionalRx.end() || !it->second->ready) { continue; }
        }
        demands.push_back({m_pendingParams.ddc0AdcIndex, slice.frequencyHz,
                           m_receiveLayout.streams()[slice.stream].centerHz});
    }
    return demands;
}
ReceiveFilterPlan AnanBackend::receiveFilterPlan() const
{
    const auto profile = receiveProfile(m_discoveredBoardId, m_pendingParams.model,
                                        m_pendingParams.overrideDetection);
    if (!profile) { return {}; }
    const auto config = m_pendingParams.filters.value_or(
        defaultFilterConfig(profile->filterKind == ReceiveFilterKind::MkIIBpf));
    const auto demands = receiveFilterDemands();
    return planReceiveFilters(config, demands, m_pendingParams.model == AnanModel::AnvelinaPro3);
}
void AnanBackend::updateReceiveStreams()
{
    if (!m_connected)
        return;
    const auto antenna = receiveAntennaRoute();
    const bool antennaChanged = m_appliedAntenna != antenna.antenna || m_appliedRxOnly != antenna.rxOnly;
    applyReceiveAntennaRouting();
    int count = 1;
    for (int st = 0; st < m_receiveLayout.limit(); ++st)
        if (m_receiveLayout.streams()[st].active)
            count = st + 1;
    std::vector<DdcConfig> configs;
    std::vector<double> centers;
    for (int st = 0; st < count; ++st) {
        const auto& w = m_receiveLayout.streams()[st];
        configs.push_back(
            {w.active ? w.rateKsps : m_pendingParams.ddc0RateKsps, m_pendingParams.ddc0AdcIndex});
        centers.push_back(w.active ? w.centerHz : frontEndReferenceHz());
    }
    std::vector<std::pair<std::shared_ptr<AdditionalRx>, int>> routes;
    for (const auto& [id, r] : m_additionalRx)
        if (const auto* s = m_receiveLayout.slice(id))
            routes.emplace_back(r, r->ready ? s->stream : -1);
    const int first = primaryVisible() ? primaryPanIndex() : 0;
    const auto primary = m_primaryRoute;
    const auto epoch = m_receiveEpoch;
    const auto writer = m_streamApplyOverride;
    const auto filterPlan = receiveFilterPlan();
    const double correction = m_receiveCalibration.frequencyFactor;
    QMetaObject::invokeMethod(
        m_client,
        [this, configs, centers, routes, first, primary, epoch, writer, filterPlan, correction] {
            if (m_receiveIoEpoch->load() != epoch)
                return;
            m_client->setReceiveFilterPlan(filterPlan);
            m_client->setFrequencyCorrection(correction);
            if (writer ? writer(configs, centers) : m_client->setReceiveStreams(configs, centers)) {
                primary->store(first);
                for (const auto& [r, st] : routes)
                    if (r->route->exchange(st) != st && st >= 0) {
                        r->seenIq.store(false);
                        QMetaObject::invokeMethod(
                            this,
                            [this, r, epoch] {
                                if (epoch == m_receiveEpoch)
                                    QTimer::singleShot(5000, this, [this, id = r->id, epoch] {
                                        checkReceiveIqTimeout(id, epoch);
                                    });
                            },
                            Qt::QueuedConnection);
                    }
            } else
                QMetaObject::invokeMethod(
                    this,
                    [this, epoch] {
                        if (epoch == m_receiveEpoch && m_connected) {
                            emit connectionError(
                                QStringLiteral("ANAN receive-stream configuration failed"));
                            disconnectRadio();
                        }
                    },
                    Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    if (antennaChanged) { publishReceiveAntennas(); }
}
bool AnanBackend::createSlice(const QString& pan, double hz)
{
    const int st = streamForPan(pan);
    return st >= 0 && addReceiveSlice(hz, st, false);
}
bool AnanBackend::createPanadapter()
{
    const auto* active = m_receiveLayout.slice(m_activeReceiveSlice);
    if (active)
        m_receiveLayout.setDefaultRate(m_receiveLayout.streams()[active->stream].rateKsps);
    return addReceiveSlice(active ? active->frequencyHz : m_sliceFreqHz, -1, true);
}
bool AnanBackend::addReceiveSlice(double hz, int requested, bool own)
{
    if (m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())) { return false; }
    if (!m_connected || !m_discoveryInfoReceived || m_rateChanging || m_receiveBuilding ||
        m_pendingBandwidthKsps != 0 || m_droopCalibrator.isRunning())
        return false;
    disableTransmitSession();
    const auto previous = m_receiveLayout;
    const auto id = m_receiveLayout.add(hz, requested, own);
    if (!id)
        return false;
    ++m_connectGeneration; // Retire any trailing single-receiver rate retries.
    const auto* slice = m_receiveLayout.slice(*id);
    auto r = std::make_shared<AdditionalRx>();
    r->id = *id;
    r->mode = m_mode;
    r->low = m_filterLowHz;
    r->high = m_filterHighHz;
    r->config = m_pendingDspConfig;
    if (const auto active = m_additionalRx.find(m_activeReceiveSlice);
        active != m_additionalRx.end()) {
        const auto& from = *active->second;
        r->mode = from.mode;
        r->low = from.low;
        r->high = from.high;
        r->agc = from.agc;
        r->threshold = from.threshold;
        r->off = from.off;
        r->nb = from.nb;
        r->nbLevel = from.nbLevel;
        r->nbFill = from.nbFill;
        r->config = from.config;
    }

    for (const auto& [otherId, other] : m_additionalRx) {
        const auto* shared = m_receiveLayout.slice(otherId);
        if (other->ready && shared && shared->stream == slice->stream) {
            r->config = other->config;
            break;
        }
    }

    r->config.inputSampleRateHz = m_receiveLayout.streams()[slice->stream].rateKsps * 1000;
    r->dsp = new AnanRxDsp;
    r->dsp->moveToThread(m_ioThread);
    m_additionalRx[*id] = r;
    m_receiveBuilding = true;
    syncReceiveAudioRoutes();
    const auto epoch = m_receiveEpoch;
    connect(
        m_client, &AnanClient::ddcIqReady, r->dsp,
        [r](int slot, const std::vector<std::complex<float>>& iq) {
            if (slot == r->route->load()) {
                r->seenIq.store(true);
                r->dsp->processIqBlock(iq);
            }
        },
        Qt::DirectConnection);
    connect(
        m_client, &AnanClient::ddcSequenceGap, r->dsp,
        [r](int slot) {
            if (slot == r->route->load())
                r->dsp->onSequenceGap();
        },
        Qt::DirectConnection);
    connect(r->dsp, &AnanRxDsp::pcmReady, m_client,
            [this, r, epoch](const PcmFrame& frame) {
                if (epoch == m_receiveIoEpoch->load() && r->route->load() >= 0) {
                    submitReceiveAudio(r->id, frame);
                }
            }, Qt::DirectConnection);
    connect(r->dsp, &AnanRxDsp::autoAgcApplied, this, [this, r, epoch](quint64 revision, double top) {
        const auto it = m_additionalRx.find(r->id);
        if (epoch == m_receiveEpoch && r->ready && it != m_additionalRx.end() && it->second == r) {
            onAutoAgcApplied(r->id, revision, top);
        }
    });
    connect(r->dsp, &AnanRxDsp::spectrumReady, this,
            [this, r, epoch](const std::vector<float>& bins) {
                if (epoch != m_receiveEpoch || !r->ready || !m_additionalRx.contains(r->id))
                    return;
                const auto* s = m_receiveLayout.slice(r->id);
                if (!s)
                    return;
                if (primaryVisible() && primaryPanIndex() == s->stream)
                    return;
                for (const auto& [id, other] : m_additionalRx) {
                    const auto* p = m_receiveLayout.slice(id);
                    if (other->ready && p && p->stream == s->stream) {
                        if (id != r->id)
                            return;
                        break;
                    }
                }
                auto values = bins;
                for (auto& v : values)
                    v += float(m_attenuationDb + m_receiveCalibration.displayOffsetDb[m_pendingParams.ddc0AdcIndex]);
                emit spectrumFrameReady(s->stream, rxBytes(values));
            });
    connect(r->dsp, &AnanRxDsp::meterUpdate, this, [this, r, epoch](float dbfs) {
        if (epoch == m_receiveEpoch && r->ready && m_additionalRx.contains(r->id))
            if (const auto level = r->sMeter.feed(double(dbfs) + m_attenuationDb
                + m_receiveCalibration.meterOffsetDb[m_pendingParams.ddc0AdcIndex]))
                emit meterUpdate(QStringLiteral("SLC%1:LEVEL").arg(r->id), *level);
    });
    std::vector<QPointer<AnanRxDsp>> running{m_dsp};
    for (const auto& [sid, other] : m_additionalRx)
        if (other->ready)
            running.push_back(other->dsp);
    const auto config = r->config;
    // All IQ processing and routing stays on the existing I/O thread. The
    // existing build thread owns planning; live controls defer while its global
    // lock is held.
    QMetaObject::invokeMethod(
        m_client,
        [this, r, epoch, config, previous, running] {
            if (m_receiveIoEpoch->load() != epoch)
                return;
            for (auto dsp : running)
                if (dsp)
                    dsp->beginRebuild();
            r->dsp->beginInitialBuild(config);
            QMetaObject::invokeMethod(
                m_dspBuildContext,
                [this, r, epoch, config, previous, running] {
                    auto result =
                        std::make_shared<AnanRxDsp::RebuildResult>(AnanRxDsp::buildChannel(config));
                    QMetaObject::invokeMethod(
                        this,
                        [this, r, epoch, result, previous, running]() mutable {
                            if (epoch != m_receiveEpoch || !m_additionalRx.contains(r->id)) {
                                releasePlanned(m_dspBuildContext, std::move(result));
                                return;
                            }
                            for (auto dsp : running)
                                if (dsp)
                                    QMetaObject::invokeMethod(
                                        dsp, [dsp] { dsp->resumeAfterPlanning(); },
                                        Qt::QueuedConnection);
                            if (!result->channel) {
                                m_receiveBuilding = false;
                                m_receiveLayout = previous;
                                m_additionalRx.erase(r->id);
                                syncReceiveAudioRoutes();
                                retireReceiveDsp(r->dsp);
                                emit sliceLifecycleFailed(QStringLiteral("create"), -1,
                                                          QString::fromStdString(result->error));
                                return;
                            }
                            QMetaObject::invokeMethod(
                                r->dsp,
                                [this, r, epoch, result] {
                                    const bool ok =
                                        r->dsp->installRebuiltChannel(std::move(*result));
                                    QMetaObject::invokeMethod(
                                        this,
                                        [this, r, epoch, ok] {
                                            if (epoch != m_receiveEpoch ||
                                                !m_additionalRx.contains(r->id))
                                                return;
                                            m_receiveBuilding = false;
                                            if (!ok) {
                                                emit connectionError(QStringLiteral(
                                                    "ANAN additional DSP install failed"));
                                                disconnectRadio();
                                                return;
                                            }
                                            r->ready = true;
                                            const auto profile = receiveProfile(
                                                m_discoveredBoardId, m_pendingParams.model,
                                                m_pendingParams.overrideDetection);
                                            seedDroopCorrectionTables(profile &&
                                                                      profile->saturnDroopDefaults);
                                            applyAdditionalRx(r->id);
                                            publishReceiveWindows();
                                            publishReceiveGain();
                                            publishAdditionalRx(r->id);
                                            updateReceiveStreams();
                                            emit capabilitiesChanged();
                                            MeterDef meter;
                                            meter.index = r->id + 10;
                                            meter.source = QStringLiteral("SLC");
                                            meter.sourceIndex = r->id;
                                            meter.name = QStringLiteral("LEVEL");
                                            meter.unit = QStringLiteral("dBm");
                                            meter.low = -140;
                                            meter.high = 20;
                                            emit meterDefined(meter);
                                        },
                                        Qt::QueuedConnection);
                                },
                                Qt::QueuedConnection);
                        },
                        Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}
void AnanBackend::applyAdditionalRx(int id)
{
    auto i = m_additionalRx.find(id);
    const auto* s = m_receiveLayout.slice(id);
    if (i == m_additionalRx.end() || !s || !i->second->ready)
        return;
    const auto r = i->second;
    const double bfo = cwBfoOffsetHz(r->mode, m_cwPitchHz),
                 shift = s->frequencyHz - m_receiveLayout.streams()[s->stream].centerHz - bfo;
    const auto mode = modeFromString(r->mode);
    const double low = r->low + bfo, high = r->high + bfo, gain = agcKnobDb(r->threshold),
                 fixed = agcKnobDb(r->off);
    const int agc = r->agc, level = r->nbLevel;
    const auto nb = AetherSDR::toWdsp(r->nb);
    const auto fill = AetherSDR::toWdsp(r->nbFill);
    QMetaObject::invokeMethod(
        r->dsp,
        [dsp = r->dsp, mode, low, high, shift, agc, gain, fixed, nb, level, fill] {
            dsp->setMode(mode);
            dsp->setFilter(low, high);
            dsp->setShift(shift);
            dsp->setAgc(agc, gain);
            dsp->setAgcFixedGain(fixed);
            dsp->setNoiseBlanker(nb, level, fill);
        },
        Qt::QueuedConnection);
    pushAutoAgc(id);
    publishAdditionalRx(id);
}
void AnanBackend::pushAdditionalShifts()
{
    for (const auto& [id, r] : m_additionalRx) {
        const auto* s = m_receiveLayout.slice(id);
        if (!s || !r->ready)
            continue;
        const double shift = s->frequencyHz - m_receiveLayout.streams()[s->stream].centerHz -
                             cwBfoOffsetHz(r->mode, m_cwPitchHz);
        QMetaObject::invokeMethod(
            r->dsp, [dsp = r->dsp, shift] { dsp->setShift(shift); }, Qt::QueuedConnection);
    }
}
void AnanBackend::publishAdditionalRx(int id)
{
    syncReceiveAudioRoutes();
    auto i = m_additionalRx.find(id);
    const auto* s = m_receiveLayout.slice(id);
    if (i == m_additionalRx.end() || !s || !i->second->ready)
        return;
    const auto& r = *i->second;
    SliceDelta d;
    d.frequency = s->frequencyHz / 1e6;
    d.letter = QString(QChar('A' + s->labelIndex));
    d.panId = rxPan(s->stream);
    d.mode = r.mode;
    d.filterLow = r.low;
    d.filterHigh = r.high;
    d.audioGain = r.gain;
    d.audioMute = r.mute;
    d.active = id == m_activeReceiveSlice;
    d.nb = r.nb != AetherSDR::NoiseBlankerKind::Off;
    d.nbKind = r.nb;
    d.nbLevel = r.nbLevel;
    d.nbFill = r.nbFill;
    d.agcMode = r.agc == 0   ? QStringLiteral("off")
                : r.agc == 2 ? QStringLiteral("slow")
                : r.agc == 4 ? QStringLiteral("fast")
                             : QStringLiteral("med");
    d.agcThreshold = r.threshold;
    d.agcThresholdMode = r.autoAgc ? AgcThresholdMode::Automatic : AgcThresholdMode::Manual;
    d.agcOffLevel = r.off;
    const auto a = activeReceiveAntenna();
    QStringList ports;
    for (std::size_t n = 0; n < a.count; ++n)
        ports.append(QString::fromLatin1(a.ports[n].data(), int(a.ports[n].size())));
    const QString current = QString::fromLatin1(a.current.data(), int(a.current.size()));
    if (m_pendingParams.filters && m_pendingParams.filters->antennaBands
        && m_pendingParams.filters->antennaBands->enabled && id != receiveAntennaOwner()) {
        ports = {current};
    }
    QStringList panPorts = ports;
    const auto* owner = m_receiveLayout.slice(receiveAntennaOwner());
    if (owner && owner->stream == s->stream) {
        panPorts.clear();
        for (std::size_t n = 0; n < a.count; ++n) {
            panPorts.append(QString::fromLatin1(a.ports[n].data(), int(a.ports[n].size())));
        }
    }
    d.rxAntennaList = ports;
    d.rxAntenna = current;
    const quint64 generation = m_connectGeneration;
    const QString pan = rxPan(s->stream);
    emit sliceChanged(id, d);
    if (!panPorts.isEmpty() && m_connected && generation == m_connectGeneration) {
        emit panAntennaListChanged(pan, panPorts);
        if (m_connected && generation == m_connectGeneration) { emit panRxAntennaChanged(pan, current); }
    }
}
ReceiveDispatch AnanBackend::requestSliceControl(int id, const SliceControlRequest& request)
{
    if (m_connected && id == 0 && request.valid()
        && (request.field == SliceControlRequest::Field::Xit || request.field == SliceControlRequest::Field::TxAntenna)) {
        if (request.field == SliceControlRequest::Field::Xit) { disableTransmitSession(); }
        emitSliceState();
        return ReceiveDispatch::Unsupported;
    }
    if (!m_connected || !m_receiveLayout.slice(id) || !request.valid() ||
        request.field != SliceControlRequest::Field::ActiveSlice)
        return ReceiveDispatch::Unsupported;
    if (std::get<bool>(request.value))
        setActiveSlice(id);
    return ReceiveDispatch::Dispatched;
}
void AnanBackend::setActiveSlice(int id)
{
    if (!m_receiveLayout.slice(id))
        return;
    m_activeReceiveSlice = id;
    emitSliceState();
    for (const auto& [sid, r] : m_additionalRx)
        publishAdditionalRx(sid);
}
bool AnanBackend::removeSlice(int id)
{
    disableTransmitSession();
    if (!m_connected || m_receiveBuilding || m_rateChanging)
        return false;
    const auto before = m_receiveLayout.streams();
    if (!m_receiveLayout.remove(id))
        return false;
    if (auto i = m_additionalRx.find(id); i != m_additionalRx.end()) {
        i->second->route->store(-1);
        retireReceiveDsp(i->second->dsp);
        m_additionalRx.erase(i);
    }
    if (m_activeReceiveSlice == id)
        m_activeReceiveSlice = m_receiveLayout.slices().begin()->first;
    emit sliceRemoved(id);
    retireEmptyReceiveWindows(before);
    updateReceiveStreams();
    setActiveSlice(m_activeReceiveSlice);
    emit capabilitiesChanged();
    return true;
}
bool AnanBackend::removePanadapter(const QString& pan)
{
    const int st = streamForPan(pan);
    if (st < 0)
        return false;
    std::vector<int> ids;
    for (const auto& [id, s] : m_receiveLayout.slices())
        if (s.stream == st)
            ids.push_back(id);
    if (ids.size() == m_receiveLayout.slices().size())
        return false;
    for (int id : ids)
        if (!removeSlice(id))
            return false;
    return true;
}
void AnanBackend::retireReceiveDsp(QPointer<AnanRxDsp> dsp)
{
    if (!dsp)
        return;
    // WDSP destruction can wait on another channel's FFTW planner. It must
    // never do so on the UDP/keepalive thread. Detach IQ on that thread,
    // then retire the QObject on the existing planning thread.
    QMetaObject::invokeMethod(
        m_client,
        [client = m_client, dsp, worker = m_dspBuildThread] {
            if (!dsp)
                return;
            QObject::disconnect(client, nullptr, dsp, nullptr);
            dsp->moveToThread(worker);
            dsp->deleteLater();
        },
        Qt::QueuedConnection);
}
void AnanBackend::clearAdditionalRx()
{
    ++m_receiveEpoch;
    m_receiveIoEpoch->store(m_receiveEpoch);
    m_receiveBuilding = false;
    m_audioRoutes.clear();
    const auto epoch = m_receiveEpoch;
    QMetaObject::invokeMethod(m_client, [this, epoch] {
        m_receiveAudioMixer.reset();
        m_audioIoEpoch = epoch;
    }, Qt::QueuedConnection);
    for (const auto& [id, r] : m_additionalRx) {
        r->route->store(-1);
        if (r->dsp)
            retireReceiveDsp(r->dsp);
    }
    m_additionalRx.clear();
    m_primaryRoute->store(0);
    m_activeReceiveSlice = 0;
}
void AnanBackend::checkReceiveIqTimeout(int id, quint64 epoch)
{
    const auto i = m_additionalRx.find(id);
    if (epoch != m_receiveEpoch || i == m_additionalRx.end() || i->second->seenIq.load())
        return;
    if (m_receiveBuilding || m_rateChanging) {
        QTimer::singleShot(250, this, [this, id, epoch] { checkReceiveIqTimeout(id, epoch); });
        return;
    }
    if (removeSlice(id))
        emit sliceLifecycleFailed(QStringLiteral("create"), id,
                                  QStringLiteral("No IQ arrived for the additional ANAN receiver"));
    else {
        emit connectionError(QStringLiteral("No IQ arrived for the remaining ANAN receiver"));
        disconnectRadio();
    }
}
void AnanBackend::syncReceiveAudioRoutes()
{
    if (!m_connected) {
        return;
    }
    std::vector<AnanAudioMixer::Route> routes;
    if (primaryVisible()) {
        routes.push_back({0, m_sliceAudioMuted, m_sliceAudioGainPercent, m_sliceAudioPanPercent});
    }
    for (const auto& [id, r] : m_additionalRx) {
        if (r->ready && m_receiveLayout.slice(id)) {
            routes.push_back({id, r->mute, r->gain, r->pan});
        }
    }
    if (routes == m_audioRoutes) {
        return;
    }
    m_audioRoutes = routes;
    const auto epoch = m_receiveEpoch;
    QMetaObject::invokeMethod(m_client, [this, routes, epoch] {
        if (epoch != m_receiveIoEpoch->load()) {
            return;
        }
        if (m_audioIoEpoch != epoch) {
            m_receiveAudioMixer.reset();
            m_audioIoEpoch = epoch;
        }
        m_receiveAudioMixer.configure(routes);
    }, Qt::QueuedConnection);
}
void AnanBackend::resetReceiveAudioBuffers()
{
    const auto epoch = m_receiveEpoch;
    QMetaObject::invokeMethod(m_client, [this, epoch] {
        if (epoch == m_receiveIoEpoch->load()) {
            m_receiveAudioMixer.resetBuffers();
        }
    }, Qt::QueuedConnection);
}
void AnanBackend::submitReceiveAudio(int id, const PcmFrame& frame)
{
    const auto epoch = m_audioIoEpoch;
    if (epoch != m_receiveIoEpoch->load()) {
        return;
    }
    const auto raw = frame.legacyStereo24();
    const auto* samples = reinterpret_cast<const float*>(raw.constData());
    if (!m_receiveAudioMixer.push(id, {samples, std::size_t(raw.size()) / sizeof(float)})) {
        return;
    }
    const auto mixed = m_receiveAudioMixer.drain();
    const auto bytes = rxBytes(mixed);
    QMetaObject::invokeMethod(this, [this, id, raw, bytes, epoch] {
        if (!m_connected || epoch != m_receiveEpoch) {
            return;
        }
        if (m_receiveLayout.slice(id)) {
            publishLegacySliceAudio(id, raw);
        }
        if (!bytes.isEmpty()) {
            publishLegacyAudio(bytes);
            sendSpeakerAudioToRadio(bytes);
        }
    }, Qt::QueuedConnection);
}
} // namespace AetherSDR::anan

namespace AetherSDR::anan {
bool AnanBackend::changeReceiveStreamRate(int stream, int rate)
{
    if (!m_connected || stream < 0 || stream >= m_receiveLayout.limit() ||
        !m_receiveLayout.streams()[stream].active) {
        return false;
    }
    if (m_receiveBuilding || m_rateChanging || m_droopCalibrator.isRunning()) {
        publishReceiveWindows();
        return false;
    }
    const bool global = m_pendingParams.protocol == 1;
    const auto before = m_receiveLayout;
    auto target = before;
    for (int st = 0; st < target.limit(); ++st) {
        const auto w = target.streams()[st];
        if (w.active && (global || st == stream) && !target.setWindow(st, w.centerHz, rate)) {
            publishReceiveWindows();
            return false;
        }
    }
    if (target.streams()[stream].rateKsps == before.streams()[stream].rateKsps) {
        // The requested span may have snapped to the existing hardware rate.
        // Confirm that rate so an optimistic view cannot retain the request.
        publishReceiveWindows();
        return true;
    }
    struct Build {
        int id;
        QPointer<AnanRxDsp> dsp;
        AnanRxDsp::Config config;
        AnanRxDsp::RebuildResult result;
    };
    auto builds = std::make_shared<std::vector<Build>>();
    const bool primary = primaryVisible() && (global || primaryPanIndex() == stream);
    if (primary) {
        auto cfg = m_pendingDspConfig;
        applyLiveRxState(cfg);
        cfg.inputSampleRateHz = rate * 1000;
        builds->push_back({0, m_dsp, cfg, {}});
    }
    std::vector<QPointer<AnanRxDsp>> running{m_dsp};
    for (const auto& [id, r] : m_additionalRx) {
        if (r->ready)
            running.push_back(r->dsp);
        const auto* s = target.slice(id);
        if (r->ready && s && (global || s->stream == stream)) {
            auto cfg = r->config;
            cfg.inputSampleRateHz = rate * 1000;
            builds->push_back({id, r->dsp, cfg, {}});
        }
    }
    m_receiveBuilding = true;
    const auto epoch = m_receiveEpoch;
    const auto rateWriter = m_rateApplyOverride;
    QMetaObject::invokeMethod(
        m_client,
        [this, builds, target, epoch, rate, primary, global, running, rateWriter] {
            if (m_receiveIoEpoch->load() != epoch)
                return;
            for (auto dsp : running)
                if (dsp)
                    dsp->beginRebuild();
            QMetaObject::invokeMethod(
                m_dspBuildContext,
                [this, builds, target, epoch, rate, primary, global, running, rateWriter] {
                    bool ok = true;
                    for (auto& b : *builds) {
                        b.result = AnanRxDsp::buildChannel(b.config);
                        if (!b.result.channel) {
                            ok = false;
                            break;
                        }
                    }
                    QMetaObject::invokeMethod(
                        this,
                        [this, builds, target, epoch, rate, primary, global, running, ok,
                         rateWriter]() mutable {
                            if (epoch != m_receiveEpoch) {
                                releasePlanned(m_dspBuildContext, std::move(builds));
                                return;
                            }
                            if (!ok) {
                                for (auto dsp : running)
                                    if (dsp)
                                        QMetaObject::invokeMethod(
                                            dsp, [dsp] { dsp->resumeAfterPlanning(); },
                                            Qt::QueuedConnection);
                                m_receiveBuilding = false;
                                publishReceiveWindows();
                                emit connectionError(
                                    QStringLiteral("ANAN DSP build failed; previous "
                                                   "receive rate retained"));
                                releasePlanned(m_dspBuildContext, std::move(builds));
                                return;
                            }
                            m_receiveLayout = target;
                            if (global || primary) {
                                m_pendingParams.ddc0RateKsps = rate;
                                m_pendingDspConfig.inputSampleRateHz = rate * 1000;
                                m_receiveLayout.setDefaultRate(rate);
                            }
                            for (const auto& b : *builds)
                                if (b.id != 0)
                                    m_additionalRx.at(b.id)->config.inputSampleRateHz = rate * 1000;
                            QMetaObject::invokeMethod(
                                m_client,
                                [this, builds, epoch, rate, global, running, rateWriter]() mutable {
                                    if (m_receiveIoEpoch->load() != epoch) {
                                        releasePlanned(m_dspBuildContext, std::move(builds));
                                        return;
                                    }
                                    for (auto& b : *builds)
                                        if (b.dsp) {
                                            b.dsp->setAudioMuted(true);
                                            b.dsp->installRebuiltChannel(std::move(b.result));
                                        }
                                    for (auto dsp : running)
                                        if (dsp)
                                            dsp->resumeAfterPlanning();
                                    const bool ok =
                                        !global || (rateWriter ? rateWriter(0, rate)
                                                               : m_client->setDdcRateLive(0, rate));
                                    QMetaObject::invokeMethod(
                                        this,
                                        [this, builds, epoch, ok] {
                                            if (epoch != m_receiveEpoch)
                                                return;
                                            if (!ok) {
                                                m_receiveBuilding = false;
                                                emit connectionError(
                                                    QStringLiteral("ANAN rate write failed"));
                                                disconnectRadio();
                                                return;
                                            }
                                            updateReceiveStreams();
                                            publishReceiveWindows();
                                            for (const auto& [id, r] : m_additionalRx) {
                                                applyAdditionalRx(id);
                                            }
                                            resetReceiveAudioBuffers();
                                            pushModeFilterShift();
                                            QTimer::singleShot(200, this, [this, builds, epoch] {
                                                if (epoch != m_receiveEpoch)
                                                    return;
                                                for (auto& b : *builds)
                                                    if (b.dsp)
                                                        QMetaObject::invokeMethod(
                                                            b.dsp,
                                                            [dsp = b.dsp] {
                                                                dsp->setAudioMuted(false);
                                                            },
                                                            Qt::QueuedConnection);
                                                m_receiveBuilding = false;
                                            });
                                        },
                                        Qt::QueuedConnection);
                                },
                                Qt::QueuedConnection);
                        },
                        Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}
} // namespace AetherSDR::anan
