#pragma once

#include "core/backends/anan/P1Client.h"

namespace AetherSDR::anan {
//DH1KLM: Backend-private protocol dispatch. Both child clients follow this
// object's affinity onto the existing ANAN IO thread; no extra thread exists.
class AnanClient : public QObject {
    Q_OBJECT
public:
    explicit AnanClient(QObject* parent = nullptr);
    struct Params : P2Client::Params { int protocol = 2; };
    using LinkCounters = P2Client::LinkCounters;
    bool start(const Params& params, int timeoutMs = 2000);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setDdc0FrequencyHz(double hz);
    Q_INVOKABLE void setFrequencyCorrection(double factor);
    Q_INVOKABLE void setSpeakerAmpMuted(bool muted);
    bool setReceiveStreams(const std::vector<DdcConfig>& streams,
                           const std::vector<double>& centers);
    Q_INVOKABLE void setFrontEndAttenuation(int adc, int db, int alexDb);
    Q_INVOKABLE void setStepAttenuationDb(int adc, int db);
    Q_INVOKABLE void setRxAntenna(int antenna);
    void setReceiveFilterPlan(const ReceiveFilterPlan& plan);
    Q_INVOKABLE void setHardwareFilters(const QVariantMap& options);
    Q_INVOKABLE void setReceiveHardware(const QVariantMap& options, int antenna);
    Q_INVOKABLE bool setDdcRateLive(int ddc, int rateKsps);
    Q_INVOKABLE void enqueueSpeakerAudio(const QByteArray& audio);
signals:
    void linkUp();
    void linkDown();
    void connectionError(const QString& reason);
    void ddcIqReady(int index, const std::vector<std::complex<float>>& block);
    void ddc0IqReady(const std::vector<std::complex<float>>& block);
    void ddcSequenceGap(int ddcIndex);
    void linkCountersUpdated(const P2Client::LinkCounters& counters);
    void discoveryInfoReceived(quint8 boardId, quint8 firmwareVer, quint8 reportedReceivers);
    void supplyRailSampled(int counts);
private:
    friend struct AnanClientTestAccess;
    P1Client* m_p1;
    P2Client* m_p2;
    int m_protocol = 2;
};
} // namespace AetherSDR::anan
