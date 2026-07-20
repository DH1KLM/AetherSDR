#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Registry of the Whisper (ggml) model tiers AetherSDR can download for the
// on-device ASR engine (RFC #4333). Weights are NOT shipped; each tier lists
// its pinned size + SHA-256 and an ordered list of download sources (Hugging
// Face primary, GitHub release-asset fallback). The model manager tries the
// sources in order and accepts a download only if its SHA-256 matches, so a
// mirror can never serve different bytes undetected.

namespace AetherSDR {

struct AsrModelTier {
    QString id;           // stable key, e.g. "base"
    QString displayName;  // UI label, e.g. "Base — 147 MB"
    QString fileName;     // on-disk + upstream name, e.g. "ggml-base.bin"
    qint64 sizeBytes = 0; // exact expected size (from the upstream LFS pointer)
    QString sha256;       // lowercase hex, pinned
    QStringList sources;  // ordered download URLs (primary first)
};

namespace AsrModelCatalog {

// All known tiers, ordered smallest → largest.
const QVector<AsrModelTier>& tiers();

// Tier by id, or nullptr if unknown.
const AsrModelTier* tierById(const QString& id);

// Platform-defaulted starting tier (RFC #4333): "base" on CPU-only builds
// (incl. ARM / Raspberry Pi); a GPU-backed build upgrades this to
// "large-v3-turbo" once a GPU ggml backend is available. Operator-overridable.
QString defaultTierId();

} // namespace AsrModelCatalog
} // namespace AetherSDR
