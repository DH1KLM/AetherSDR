# Copy Assist — On-device Speech-to-Text (ASR)

Copy Assist transcribes received **voice** (SSB/AM/FM) into a live, scrolling
text panel docked under the waterfall — an assist for weak/noisy copy,
accessibility, and nets/contests. It is **receive-only** and never keys TX.

Design + decision record: RFC **#4333** (accepted). Engine: **whisper.cpp**
(MIT). This document is the user + contributor reference.

## Using it

- Open with the **`ASR`** toggle in the status bar (between `CWX` and `DVK`),
  which shows/hides the panel. The toggle is the inverse of `CWX`:
  **enabled in voice modes** (USB/LSB/AM/SAM/FM/NFM/DFM), **dimmed in CW and
  DIGx/RTTY**.
- Tick **Enable**. On first enable the selected model is downloaded (see below),
  verified, and loaded; a loading indicator shows progress. Then transcription
  of the audio you're hearing streams into the panel.
- Text is **color-coded by recognition confidence**: green (high) → yellow →
  orange → red (low), mirroring the CW decoder.
- Hiding the panel (the status-bar **ASR** toggle / leaving voice mode) turns
  ASR off.

### Settings (⚙)

The **⚙ button** (next to Enabled) opens a small modeless **settings dialog**
holding the **model** and **compute-device** (GPU/CPU) pickers, with room for
more options. It floats over the app and can stay open while you operate.

### Tuning (the control row)

| Control | Range | Effect |
|---|---|---|
| **Buffer** | 1–20 s | Max audio accumulated before a decode is forced without a silence gap. |
| **Sensitivity** | 1–100 % | VAD threshold — higher picks up fainter/weaker speech. |
| **Silence** | 100–2000 ms | Trailing silence that ends an utterance. |

All three, plus the panel height, persist client-side in `AppSettings`.

## Models

Weights are **not bundled** — they download on first enable and cache under
`QStandardPaths::AppDataLocation/models` (e.g. `~/.local/share/AetherSDR/models/`).
Sources are tried in order and each download is **SHA-256-verified** before it
is accepted:

1. **Hugging Face** — `huggingface.co/ggerganov/whisper.cpp` (primary)
2. **GitHub release** — `aethersdr/AetherSDR` tag `asr-models-v1` (mirror fallback)

| Tier | Size | Notes |
|---|---|---|
| tiny | 74 MB | fastest, roughest |
| **base** | 141 MB | default on CPU/ARM (incl. Raspberry Pi 5) |
| small | 465 MB | desktop CPU |
| **large-v3-turbo** | 1.6 GB | default when a GPU is available |

Offline/air-gapped: drop the `ggml-*.bin` file into the models dir manually.

### Bring your own model

To use a model that isn't in the tier list — a fine-tune, a different
quantization, or a manually-downloaded `ggml-*.bin`/`.gguf` — select
**"Custom model…"** in the model picker (⚙ settings) and pick the file. It loads directly
(no download, no checksum), the picker remembers it (shown as `Custom: <name>`),
and it runs on the selected compute device like any built-in tier.

## GPU acceleration

The selected model runs on the **GPU when one is available**, else CPU
(automatic fallback). GPU is auto-detected at build time and used at runtime via
`ggml_backend_dev_by_type(GPU)`:

- **Vulkan** — Linux/Windows (NVIDIA/AMD/Intel). Requires the Vulkan
  loader+headers, `glslc`, and `SPIRV-Headers` at build time (`ENABLE_ASR_VULKAN`,
  auto).
- **Metal** — macOS (native). Uses the Metal framework + `metal` compiler from
  full Xcode (`ENABLE_ASR_METAL`, auto on Apple).

Without the toolchain the build is CPU-only, unchanged. A GPU-enabled binary
still runs on GPU-less hosts.

## Remote backend (bring your own server)

Instead of the bundled engine, Copy Assist can offload transcription to a
**user-configured OpenAI-compatible `/v1/audio/transcriptions` endpoint** —
whisper.cpp's `whisper-server`, faster-whisper, or any compatible server — to
run inference on another machine or experiment with different engines.

Select **"Remote server…"** in the model picker (⚙ settings) and enter the endpoint URL
(e.g. `http://host:8080/v1/audio/transcriptions`), an optional API key, and the
model name. AetherSDR ships **no server and no default endpoint**; cloud
endpoints are entirely opt-in and user-configured.

## Architecture (contributors)

ASR lives in its own static library **`aetherasr`** (Qt Core/Network +
whisper) — **not** in `libaethercore`, which stays whisper-free (verified: 0
whisper symbols) so a thin UI / headless engine never links it.

```
AudioEngine (aethercore, 24 kHz post-NR RX)
   └─ AsrAudioTap (gui)  ── mono → ──▶ AsrEngine (aetherasr)
                                          ├─ worker thread: resample 24k→16k (r8brain)
                                          ├─ AsrSegmenter (energy VAD → utterances)
                                          └─ IAsrBackend
                                               ├─ WhisperAsrBackend (local, CPU/Vulkan/Metal)
                                               └─ RemoteAsrBackend (HTTP endpoint)
   AsrEngine::finalText(text, confidence) ──▶ CopyAssistPanel (gui, color-coded)
```

- **`IAsrBackend`** is the pluggable seam; `WhisperAsrBackend` and
  `RemoteAsrBackend` implement it. `AsrEngine` is backend-agnostic and testable
  with a fake backend.
- **`AsrEngine::finalText`** is the stream seam the UI subscribes to (a signal
  today; over the aetherd wire later — the thin UI never links whisper).
- All inference + resampling + verification run off the audio/UI threads.

### Build flags

| Flag | Default | Effect |
|---|---|---|
| `ENABLE_ASR` | ON | Build ASR (`aetherasr` + Copy Assist). `OFF` = no ASR. |
| `ENABLE_ASR_VULKAN` | ON (auto) | Vulkan GPU backend when the toolchain is present (non-Apple). |
| `ENABLE_ASR_METAL` | ON (Apple) | Metal GPU backend (macOS). |

Vendored whisper.cpp is pinned; see
[`third_party/whisper.cpp/AETHER_VENDORING.md`](../third_party/whisper.cpp/AETHER_VENDORING.md).

### Tests

`ctest --test-dir build -R 'asr_|copy_assist'` — all offline/CI-safe: segmenter,
engine (fake backend), model manager (source failover + hash-mismatch), Copy
Assist panel (confidence coloring), settings dialog (model + GPU pickers),
remote backend (mock endpoint), whisper linkage. Real GPU/CPU inference is exercised by the env-gated
`asr_whisper_backend_test` (`AETHER_ASR_TEST_MODEL` + `AETHER_ASR_TEST_PCM`).
