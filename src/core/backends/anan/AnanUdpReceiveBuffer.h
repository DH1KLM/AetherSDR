#pragma once

#include <QUdpSocket>
#include <QVariant>
#include <optional>

namespace AetherSDR::anan {
// Thetis network.c uses SO_RCVBUF = 0xFA000. Read back the OS result after bind.
inline constexpr int kUdpReceiveBufferBytes = 0xFA000;
inline std::optional<qint64> requestUdpReceiveBuffer(QUdpSocket& socket)
{
    socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,kUdpReceiveBufferBytes);
    bool valid = false;
    const qint64 granted = socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong(&valid);
    return valid && granted > 0 ? std::optional<qint64>{granted} : std::nullopt;
}
} // namespace AetherSDR::anan
