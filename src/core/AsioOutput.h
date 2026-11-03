#pragma once

#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QList>
#include <memory>

namespace AetherSDR {
struct AsioOutputConfig {
    bool enabled = false;
    QString driver;
    int firstChannel = 0;
    int sampleRate = 48000;
    int bufferFrames = 0;
    bool microphoneEnabled = false;
    int firstInputChannel = 0;
    int inputMode = 0;
    static AsioOutputConfig load();
    bool save() const;
    bool valid() const;
};
struct AsioOutputDriver { QString name; QString id; };

// Shared input/output driver session, owned by the AudioEngine thread.
class AsioOutput final : public QIODevice {
public:
    explicit AsioOutput(QObject* parent = nullptr);
    ~AsioOutput() override;
    static QList<AsioOutputDriver> drivers();
    bool start(const AsioOutputConfig& config, QString& error);
    void stop();
    bool flush();
    void setGain(float value);
    qsizetype bytesFree() const;
    qsizetype bufferBytes() const;
    int deviceBufferFrames() const;
    bool resetRequested() const;
    bool inputFaulted() const;
    QIODevice* inputDevice() const;
    void discardInput();
    void setInputActive(bool active);
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* bytes, qint64 size) override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
