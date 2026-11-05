#include "AnanBackend.h"
#include "AnanPlannedResource.h"

#include <QMetaObject>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace AetherSDR::anan {
void AnanBackend::requestPureSignal(const QString& verb, quint64 requestId, const QVariantMap& fields)
{
    const bool save = verb == QLatin1String("puresignal.save");
    const bool restore = verb == QLatin1String("puresignal.restore");
    const bool write = verb != QLatin1String("puresignal.get");
    const QString path = fields.value(QStringLiteral("path")).toString();
    QString reason;
    const auto profile = receiveProfile(m_discoveredBoardId);
    if (!m_connected || !m_discoveryInfoReceived) {
        reason = QStringLiteral("Connect a validated OpenHPSDR device first.");
    } else if (!profile || profile->numAdcs != 2) {
        reason = QStringLiteral("Pure Signal feedback currently requires a two-ADC OpenHPSDR device.");
    } else if (m_pendingParams.protocol == 1 && m_pendingParams.ddc0RateKsps != 192) {
        reason = QStringLiteral("Select a 192 kHz receive sample rate before enabling Protocol 1 Pure Signal.");
    } else if (!transmitSessionCurrent()) {
        reason = QStringLiteral("Enable and prepare the TX session in Radio Setup before configuring Pure Signal.");
    } else if (m_diversity.enabled || m_diversityPending) {
        reason = QStringLiteral("Disable diversity before reserving the Pure Signal feedback DDCs.");
    } else if (m_receiveBuilding || m_rateChanging || m_receiveCalibrationRun || m_droopCalibrator.isRunning()
        || m_pureSignalPending || m_txSessionArming) {
        reason = QStringLiteral("Wait for the current radio configuration to finish.");
    }
    PureSignalSettings settings;
    PureSignalMode mode = PureSignalMode::Off;
    if (write) {
        if (!reason.isEmpty() || m_transmitOperation.permitsCleanup()) {
            emit extensionError(requestId, reason.isEmpty()
                ? QStringLiteral("Finish transmission before changing Pure Signal.") : reason);
            return;
        }
        const QStringList names{QStringLiteral("moxWaitSeconds"), QStringLiteral("calibrationWaitSeconds"),
            QStringLiteral("amplifierDelayNs"), QStringLiteral("hardwarePeak"), QStringLiteral("mode")};
        bool valid = fields.value(QStringLiteral("identity")).metaType().id() == QMetaType::QString
            && fields.value(QStringLiteral("identity")).toString() == transmitPreparationIdentity();
        if (save || restore) {
            valid = valid && fields.value(QStringLiteral("path")).metaType().id() == QMetaType::QString
                && path.size() <= 32767 && !path.contains(QChar::Null) && QFileInfo(path).isAbsolute();
        }
        const bool hasAutoAttenuate = !save && fields.contains(QStringLiteral("autoAttenuate"));
        valid = valid && fields.size() == (save ? 2 : restore ? 7 : 6) + (hasAutoAttenuate ? 1 : 0);
        if (hasAutoAttenuate) {
            valid = valid && fields.value(QStringLiteral("autoAttenuate")).metaType().id() == QMetaType::Bool;
            settings.autoAttenuate = fields.value(QStringLiteral("autoAttenuate")).toBool();
        }
        if (save) {
            if (!valid) { emit extensionError(requestId,QStringLiteral("Invalid correction export or retired connection.")); return; }
            m_pureSignalPending = true;
        } else {
            for (const QString& name : names) {
                const QVariant value = fields.value(name);
                const int type = value.metaType().id();
                valid = valid && (type == QMetaType::Double || type == QMetaType::Int || type == QMetaType::LongLong)
                    && std::isfinite(value.toDouble());
            }
            const double modeValue = fields.value(QStringLiteral("mode")).toDouble();
            valid = valid && modeValue >= 0 && modeValue <= 3 && std::floor(modeValue) == modeValue
                && (!restore || modeValue == 3);
            settings.moxWaitSeconds = fields.value(names[0]).toDouble();
            settings.calibrationWaitSeconds = fields.value(names[1]).toDouble();
            settings.amplifierDelayNs = fields.value(names[2]).toDouble();
            settings.hardwarePeak = fields.value(names[3]).toDouble();
            if (!valid || !settings.valid()) {
                emit extensionError(requestId, QStringLiteral("Invalid Pure Signal settings or retired connection."));
                return;
            }
            mode = static_cast<PureSignalMode>(static_cast<int>(modeValue));
            m_pureSignalPending = true;
        }
    }
    const quint64 epoch = m_receiveEpoch;
    const QString identity = transmitPreparationIdentity();
    const bool display = fields.value(QStringLiteral("display")).toBool();
    const auto permit = m_txSessionPermit;
    const auto revision = m_txFilterRevision;
    const quint64 filterRevision = revision->load();
    const auto dispatch = [this, epoch, requestId, write, mode, settings, display, identity, reason,
                           permit, revision, filterRevision, save](std::shared_ptr<AnanTxDsp> prepared, bool saved = false) {
      QMetaObject::invokeMethod(m_client, [this, epoch, requestId, write, mode, settings, display, identity, reason,
                                          permit, revision, filterRevision, save, saved, prepared = std::move(prepared)] {
        if (m_receiveIoEpoch->load() != epoch) { return; }
        const bool accepted = !write || (permit && permit->load() && revision->load() == filterRevision
            && (save ? saved : m_client->configurePureSignal(settings, mode, prepared)));
        QVariantMap state = m_client->pureSignalState(display);
        QMetaObject::invokeMethod(this, [this, epoch, requestId, write, mode, accepted, identity, reason, state]() mutable {
            if (m_receiveEpoch != epoch || !m_connected) { return; }
            if (write) { m_pureSignalPending = false; }
            if (identity != transmitPreparationIdentity()) {
                emit extensionError(requestId,QStringLiteral("Pure Signal operation retired after a TX configuration change."));
                return;
            }
            if (!accepted) {
                emit extensionError(requestId, QStringLiteral("Pure Signal operation refused: check the current session and a valid WDSP v2 correction."));
                return;
            }
            const bool enabled = state.value(QStringLiteral("mode")).toInt() != 0;
            if (enabled != m_pureSignalEnabled) {
                m_pureSignalEnabled = enabled;
                updateReceiveStreams();
            }
            state.insert(QStringLiteral("identity"), identity);
            state.insert(QStringLiteral("available"), reason.isEmpty());
            state.insert(QStringLiteral("reason"), reason);
            emit extensionResult(requestId, state);
        }, Qt::QueuedConnection);
      }, Qt::QueuedConnection);
    };
    if (!write) { dispatch({}); return; }
    const auto txMode = modeFromString(transmitModeName());
    const int outputRate = m_pendingParams.protocol == 1 ? 48000 : 192000;
    const auto recipe = effectiveTransmitMode();
    const auto passband = recipe ? std::optional(std::pair(recipe->filterLowHz, recipe->filterHighHz)) : std::nullopt;
    QMetaObject::invokeMethod(m_client, [this, epoch, txMode, settings, mode, passband, dispatch, outputRate,
                                        save, restore, path, permit, revision, filterRevision] {
      if (m_receiveIoEpoch->load() != epoch) { return; }
      const std::shared_ptr<AnanTxDsp> source = m_client->pureSignalDspForControl();
      QMetaObject::invokeMethod(m_dspBuildContext, [this, epoch, txMode, settings, mode, passband, dispatch, outputRate,
                                                  save, restore, path, source, permit, revision, filterRevision] {
        if (m_receiveIoEpoch->load() != epoch) { return; }
        const auto current = [this, epoch, permit, revision, filterRevision] {
            return m_receiveIoEpoch->load() == epoch && permit && permit->load()
                && revision->load() == filterRevision;
        };
        if (save) {
            const auto bytes = source && source->pureSignalChannel()
                ? source->pureSignalChannel()->savePureSignalCorrection() : std::nullopt;
            QSaveFile output(path);
            const bool saved = bytes && current() && output.open(QIODevice::WriteOnly)
                && output.write(bytes->data(),static_cast<qint64>(bytes->size())) == static_cast<qint64>(bytes->size())
                && current() && output.commit();
            dispatch({}, saved);
            return;
        }
        auto dsp = std::make_unique<AnanTxDsp>();
        bool ready = current() && dsp->configure(txMode, nullptr, outputRate, passband)
            && dsp->pureSignalChannel()->configurePureSignal(settings, mode, 192000);
        if (ready && mode == PureSignalMode::Current) {
            if (restore) {
                QFile input(path);
                ready = input.open(QIODevice::ReadOnly) && input.size() > 0 && input.size() <= 1024*1024;
                if (ready) { ready = dsp->pureSignalChannel()->restorePureSignalCorrection(input.readAll().toStdString()); }
            } else {
                ready = source && source->pureSignalChannel()
                    && dsp->pureSignalChannel()->copyPureSignalCorrectionFrom(*source->pureSignalChannel());
            }
        }
        dispatch(ready ? retainPlannedResource(std::move(dsp), m_dspBuildContext) : nullptr);
      }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
} // namespace AetherSDR::anan
