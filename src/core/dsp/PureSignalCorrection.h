#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {
// Validate the complete WDSP v2 document before its compatibility reader may
// allocate any branches. The file is also bounded before reading it from disk.
[[nodiscard]] bool validatePureSignalCorrection(const QByteArray& document, QString* error = nullptr);
} // namespace AetherSDR
