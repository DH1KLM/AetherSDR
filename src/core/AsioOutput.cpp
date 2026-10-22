#include "AsioOutput.h"
#include "AsioPlaybackBuffer.h"
#include "AppSettings.h"

#include <QJsonDocument>
#include <QVariant>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <objbase.h>
#include "../../third_party/asiosdk/iasiodrv.h"
#endif

namespace AetherSDR {
AsioOutputConfig AsioOutputConfig::load()
{
    const QJsonObject doc = QJsonDocument::fromJson(
        AppSettings::instance().value("AsioOutput").toByteArray()).object();
    if (doc.isEmpty()) { return {}; }
    AsioOutputConfig config;
    if (doc.value("version").toInt() != 1) { return config; }
    config.enabled = doc.value("enabled").toBool();
    config.driver = doc.value("driver").toString();
    config.firstChannel = doc.value("firstChannel").toInt(-1);
    config.sampleRate = doc.value("sampleRate").toInt();
    config.bufferFrames = doc.value("bufferFrames").toInt(-1);
    return config.valid() ? config : AsioOutputConfig{};
}
bool AsioOutputConfig::valid() const
{
    return (!enabled || !driver.isEmpty()) && firstChannel >= 0 && firstChannel < 255
        && (sampleRate == 44100 || sampleRate == 48000 || sampleRate == 96000
            || sampleRate == 192000) && bufferFrames >= 0 && bufferFrames <= 8192;
}
bool AsioOutputConfig::save() const
{
    if (!valid()) { return false; }
    AppSettings& settings = AppSettings::instance();
    if (!settings.storeWritable()) { return false; }
    const QVariant previous = settings.value("AsioOutput");
    const QJsonObject old = QJsonDocument::fromJson(settings.value("AsioOutput").toByteArray()).object();
    if (!old.isEmpty() && old.value("version").toInt() != 1) { return false; }
    const QJsonObject doc{{"version", 1}, {"enabled", enabled}, {"driver", driver},
        {"firstChannel", firstChannel}, {"sampleRate", sampleRate}, {"bufferFrames", bufferFrames}};
    const QString value = QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
    settings.setValue("AsioOutput", value);
    settings.save();
    QString persisted;
    if (settings.readAppRowFromDisk("AsioOutput", persisted) && persisted == value) { return true; }
    if (previous.isValid()) { settings.setValue("AsioOutput", previous); }
    else { settings.remove("AsioOutput"); }
    return false;
}

struct AsioOutput::Impl {
    std::unique_ptr<AsioPlaybackBuffer> ring;
    std::atomic<float> gain{1};
    std::atomic_bool reset{false};
    int frames = 0;
    std::size_t capacity = 0;
#ifdef Q_OS_WIN
    IASIO* driver = nullptr;
    bool comOwned = false;
    bool buffersCreated = false;
    bool started = false;
    std::atomic_bool ready{false};
    int sampleRate = 0;
    std::array<ASIOBufferInfo, 2> buffers{};
    std::array<int, 2> types{};
    static std::atomic<Impl*> active;
    static void switched(long index, ASIOBool)
    {
        Impl* state = active.load(std::memory_order_acquire);
        if (!state || !state->ready.load(std::memory_order_acquire) || index < 0 || index > 1) { return; }
        const float gain = state->reset.load(std::memory_order_relaxed)
            ? 0 : state->gain.load(std::memory_order_relaxed);
        for (int frame = 0; frame < state->frames; ++frame) {
            const std::array<float, 2> sample = state->ring->read();
            for (int channel = 0; channel < 2; ++channel) {
                auto* dst = static_cast<std::uint8_t*>(state->buffers[channel].buffers[index]);
                AsioPlaybackBuffer::encode(sample[channel] * gain, state->types[channel],
                    dst + frame * AsioPlaybackBuffer::sampleBytes(state->types[channel]));
            }
        }
        state->driver->outputReady();
    }
    static ASIOTime* switchedTime(ASIOTime* time, long index, ASIOBool direct)
    { switched(index, direct); return time; }
    static void rateChanged(ASIOSampleRate rate)
    {
        Impl* state = active.load(std::memory_order_acquire);
        if (state && state->ready.load(std::memory_order_acquire) && rate != state->sampleRate) {
            state->reset.store(true);
        }
    }
    static long message(long selector, long value, void*, double*)
    {
        switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion || value == kAsioResetRequest
                || value == kAsioBufferSizeChange || value == kAsioResyncRequest
                || value == kAsioSupportsTimeInfo;
        case kAsioEngineVersion: return 2;
        case kAsioSupportsTimeInfo: return 1;
        case kAsioResetRequest: case kAsioBufferSizeChange: case kAsioResyncRequest:
            if (Impl* state = active.load(std::memory_order_acquire)) { state->reset.store(true); }
            return 1;
        default: return 0;
        }
    }
    ASIOCallbacks callbacks{switched, rateChanged, message, switchedTime};
#endif
};
#ifdef Q_OS_WIN
std::atomic<AsioOutput::Impl*> AsioOutput::Impl::active{nullptr};
#endif

AsioOutput::AsioOutput(QObject* parent) : QIODevice(parent), m_impl(std::make_unique<Impl>()) {}
AsioOutput::~AsioOutput() { stop(); }
QList<AsioOutputDriver> AsioOutput::drivers()
{
    QList<AsioOutputDriver> result;
#ifdef Q_OS_WIN
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return result;
    }
    for (DWORD index = 0; index < 256; ++index) {
        std::array<wchar_t, 256> name{};
        DWORD count = static_cast<DWORD>(name.size());
        const LSTATUS status = RegEnumKeyExW(root, index, name.data(), &count, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) { break; }
        if (status != ERROR_SUCCESS) { continue; }
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, name.data(), 0, KEY_READ, &key) != ERROR_SUCCESS) { continue; }
        std::array<wchar_t, 128> clsid{};
        DWORD bytes = sizeof(clsid);
        const LSTATUS read = RegGetValueW(key, nullptr, L"CLSID", RRF_RT_REG_SZ,
            nullptr, clsid.data(), &bytes);
        RegCloseKey(key);
        if (read == ERROR_SUCCESS) {
            result.append({QString::fromWCharArray(name.data()), QString::fromWCharArray(clsid.data())});
        }
    }
    RegCloseKey(root);
#endif
    return result;
}
bool AsioOutput::start(const AsioOutputConfig& config, QString& error)
{
    stop();
    if (!config.valid() || !config.enabled) { error = tr("Invalid ASIO output configuration."); return false; }
#ifdef Q_OS_WIN
    Impl& state = *m_impl;
    Impl* expected = nullptr;
    if (!Impl::active.compare_exchange_strong(expected, &state)) {
        error = tr("Another ASIO output is already open."); return false;
    }
    const auto fail = [this, &error](const QString& reason) { error = reason; stop(); return false; };
    const QList<AsioOutputDriver> installed = drivers();
    const auto match = std::find_if(installed.begin(), installed.end(), [&config](const auto& driver) {
        return driver.id == config.driver;
    });
    if (match == installed.end()) { return fail(tr("The selected ASIO driver is not installed (64-bit).")); }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    state.comOwned = SUCCEEDED(com);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) { return fail(tr("ASIO COM initialization failed.")); }
    CLSID id{};
    if (FAILED(CLSIDFromString(reinterpret_cast<LPCOLESTR>(config.driver.utf16()), &id))
        || FAILED(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id,
            reinterpret_cast<void**>(&state.driver))) || !state.driver || !state.driver->init(nullptr)) {
        return fail(tr("ASIO driver initialization failed. Close other applications using this driver."));
    }
    long inputs = 0, outputs = 0, minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    if (state.driver->getChannels(&inputs, &outputs) != ASE_OK || config.firstChannel + 1 >= outputs
        || state.driver->getBufferSize(&minimum, &maximum, &preferred, &granularity) != ASE_OK) {
        return fail(tr("The ASIO driver does not provide the selected stereo pair or buffer limits."));
    }
    const long frames = config.bufferFrames == 0 ? preferred : config.bufferFrames;
    if (minimum < 1 || maximum < minimum || granularity < -1 || frames < minimum || frames > maximum || frames > 8192
        || (granularity == -1 && !std::has_single_bit(static_cast<unsigned long>(frames)))
        || (granularity > 0 && (frames - minimum) % granularity != 0)
        || (granularity == 0 && frames != preferred)) {
        return fail(tr("The ASIO driver does not support the selected buffer size."));
    }
    if (state.driver->canSampleRate(config.sampleRate) != ASE_OK
        || state.driver->setSampleRate(config.sampleRate) != ASE_OK) {
        return fail(tr("The ASIO driver refused the selected sample rate."));
    }
    ASIOSampleRate actual = 0;
    if (state.driver->getSampleRate(&actual) != ASE_OK || actual != config.sampleRate) {
        return fail(tr("The ASIO driver did not apply the requested sample rate."));
    }
    state.frames = static_cast<int>(frames);
    state.sampleRate = config.sampleRate;
    state.capacity = static_cast<std::size_t>(std::max<long>(frames * 2, config.sampleRate / 50));
    state.ring = std::make_unique<AsioPlaybackBuffer>(state.capacity);
    for (int channel = 0; channel < 2; ++channel) {
        state.buffers[channel] = {ASIOFalse, config.firstChannel + channel, {nullptr, nullptr}};
    }
    if (state.driver->createBuffers(state.buffers.data(), 2, frames, &state.callbacks) != ASE_OK) {
        return fail(tr("ASIO output buffer creation failed."));
    }
    state.buffersCreated = true;
    for (int channel = 0; channel < 2; ++channel) {
        ASIOChannelInfo info{};
        info.channel = config.firstChannel + channel;
        info.isInput = ASIOFalse;
        if (state.driver->getChannelInfo(&info) != ASE_OK || !AsioPlaybackBuffer::sampleBytes(info.type)) {
            return fail(tr("Unsupported ASIO sample format; use a little-endian PCM/float driver."));
        }
        state.types[channel] = static_cast<int>(info.type);
        for (void* buffer : state.buffers[channel].buffers) {
            if (!buffer) { return fail(tr("ASIO returned an empty output buffer.")); }
            std::memset(buffer, 0, frames * AsioPlaybackBuffer::sampleBytes(info.type));
        }
    }
    state.reset.store(false);
    state.ready.store(true, std::memory_order_release);
    state.started = true;
    if (state.driver->start() != ASE_OK) { return fail(tr("ASIO playback start failed.")); }
    open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    error.clear();
    return true;
#else
    error = tr("ASIO output is available on Windows only.");
    return false;
#endif
}
void AsioOutput::stop()
{
    close();
    Impl& state = *m_impl;
#ifdef Q_OS_WIN
    if (state.driver) {
        if (state.started) { state.driver->stop(); }
        state.started = false;
        state.ready.store(false, std::memory_order_release);
        Impl* current = &state;
        Impl::active.compare_exchange_strong(current, nullptr);
        if (state.buffersCreated) { state.driver->disposeBuffers(); }
        state.buffersCreated = false;
        state.driver->Release();
        state.driver = nullptr;
    }
    Impl* expected = &state;
    Impl::active.compare_exchange_strong(expected, nullptr);
    if (state.comOwned) { CoUninitialize(); state.comOwned = false; }
#endif
    state.ring.reset();
    state.capacity = 0;
}
bool AsioOutput::flush()
{
#ifdef Q_OS_WIN
    Impl& state = *m_impl;
    if (!state.driver || !state.started || state.reset.load()) { return false; }
    if (state.driver->stop() != ASE_OK) { return false; }
    state.started = false;
    state.ring->reset();
    for (int channel = 0; channel < 2; ++channel) {
        for (void* buffer : state.buffers[channel].buffers) {
            std::memset(buffer, 0, state.frames * AsioPlaybackBuffer::sampleBytes(state.types[channel]));
        }
    }
    state.started = state.driver->start() == ASE_OK;
    return state.started;
#else
    return false;
#endif
}
void AsioOutput::setGain(float value) { m_impl->gain.store(std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0); }
qsizetype AsioOutput::bytesFree() const { return m_impl->ring ? static_cast<qsizetype>(m_impl->ring->freeFrames() * 8) : 0; }
qsizetype AsioOutput::bufferBytes() const { return static_cast<qsizetype>(m_impl->capacity * 8); }
int AsioOutput::deviceBufferFrames() const { return m_impl->frames; }
bool AsioOutput::resetRequested() const { return m_impl->reset.load(); }
qint64 AsioOutput::writeData(const char* bytes, qint64 size)
{
    if (!isOpen() || !m_impl->ring || size < 0 || size % 8 != 0 || resetRequested()) { return -1; }
    const std::size_t count = std::min(static_cast<std::size_t>(size / 8), m_impl->ring->freeFrames());
    for (std::size_t i = 0; i < count; ++i) {
        //DH1KLM: QIODevice accepts unaligned byte storage; only the local frame is read as float.
        std::array<float, 2> frame{};
        std::memcpy(frame.data(), bytes + i * 8, 8);
        m_impl->ring->write(frame);
    }
    return static_cast<qint64>(count * 8);
}
}
