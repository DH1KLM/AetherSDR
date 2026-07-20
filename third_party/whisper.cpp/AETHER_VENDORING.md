# Vendored whisper.cpp — AetherSDR ASR engine

Upstream: <https://github.com/ggml-org/whisper.cpp>
Version: **1.9.1** — commit pinned in [`COMMIT`](COMMIT)
License: MIT (see [`LICENSE`](LICENSE)) — GPL-v3-compatible; attribute in the
third-party notices, do **not** modify vendored sources in place.

This is the ASR engine adopted in RFC #4333. Weights are **not** vendored — they
are downloaded on first enable (primary Hugging Face, fallback GitHub release
asset, SHA-256 pinned). See the RFC for the model-manager design.

## What was trimmed from the upstream tree

To keep the checkout small, only the **library** is vendored. Removed:

- Non-library top level: `examples/`, `tests/`, `bindings/`, `models/`,
  `media/`, `samples/`, `grammars/`, `ci/`, `scripts/`, `Makefile`,
  `CMakePresets.json`, `*.yml`, `build-xcframework.sh`, README variants.
- **GPU / accelerator ggml backends** (Phase 1 is Linux-CPU only):
  `ggml-cuda`, `ggml-hip`, `ggml-musa`, `ggml-vulkan`, `ggml-webgpu`,
  `ggml-sycl`, `ggml-opencl`, `ggml-metal`, `ggml-openvino`, `ggml-cann`,
  `ggml-rpc`, `ggml-virtgpu`, `ggml-zdnn`, `ggml-zendnn`, `ggml-hexagon`.

Kept: `include/`, `src/` (whisper), `cmake/`, and `ggml/` with the **CPU**
backend (`ggml-cpu`, plus the un-compiled `ggml-blas` source). Each dropped
backend is guarded by `if(GGML_<X>)` upstream, so with those options OFF the
missing directories are never referenced.

## Re-vendoring / adding a GPU backend

A later RFC phase that enables a GPU backend must **re-copy that backend's
directory** from upstream at the pinned commit and turn its `GGML_<X>` option
ON in the top-level `CMakeLists.txt` ASR block (with the matching CI runner).
To refresh: clone upstream at `COMMIT`, re-run the same trim, and diff.
AetherSDR forces CPU-only + `GGML_NATIVE=OFF` for portable/Pi/CI binaries.
