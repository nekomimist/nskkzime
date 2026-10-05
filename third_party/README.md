# Vendored dependencies

## nlohmann/json

- Version: 3.12.0
- Source: https://github.com/nlohmann/json/releases/tag/v3.12.0
- Header: `nlohmann/json.hpp` (unchanged single header)
- SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
- License: [MIT](nlohmann/LICENSE.MIT)

The checksum was verified against the upstream release. Builds do not download dependencies.

## skk-zenz

- Source: https://github.com/nekomimist/skk-zenz/tree/db1c923223a94c6406fd19e4d75ef3b54bdf9300
- Copied: `model.cpp`, `model.h`, `prompt.cpp`, `prompt.h`, and their tests under `skk-zenz/`.
- License: [MIT](skk-zenz/LICENSE), Copyright (c) 2026 Hiroyuki Ishikura.
- Source archive SHA-256: `434c090f47934fecdb54304a871e17f7fed423d65a1c5138b1754b08402b0c7c`.
- Local changes: model abort callback, cancellation checks in generation/scoring and before decode. Prompt and upstream tests are unchanged.
- The original protocol implementation is not copied. Our bounded protocol supports its `convert` and `score` concepts, with the differences documented in [INFERENCE.md](../docs/INFERENCE.md).

## azooKey llama.cpp (optional CPU backend)

- Source: https://github.com/azooKey/llama.cpp/tree/66afb885a8ddcc0511798aec9fb5a01017941018
- This is the skk-zenz submodule revision at the commit above, on `azookey/b9637-compat`.
- License: [MIT](llama.LICENSE), Copyright (c) 2023–2026 The ggml authors.
- Archive SHA-256: `df8e483c6bb30e358389f6be382a63f14bd8648d327b4994fc2f0b1df63ff97b`.
- `scripts/prepare-zenz.ps1` downloads and verifies the archive into ignored `out/deps/`. Builds themselves do not download anything.
- Sources are unchanged. Parent CMake selects C++17 for llama.cpp and C++20 for our code, and disables GPU/OpenMP/native CPU detection. llama.cpp is linked only into the optional server and model tests.

## zenz model (not bundled)

- Repository: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/tree/c67e03e07d215c869f591b274c1631170d3e11fe
- File: `ggml-model-Q5_K_M.gguf`.
- SHA-256: `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`.
- The pinned model card declares Apache-2.0. The preparation script downloads the card alongside the model into ignored `out/models/`.
- Neither model weights nor a model distribution package are included in this repository. Distribution must include the applicable model notices and license.
