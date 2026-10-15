# Vendored dependencies

## nlohmann/json

- Version: 3.12.0
- Source: https://github.com/nlohmann/json/releases/tag/v3.12.0
- Header: `nlohmann/json.hpp` (unchanged single header)
- SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
- License: [MIT](nlohmann/LICENSE.MIT)

The checksum was verified against the upstream release. Builds do not download dependencies.

## Unicode JIS X 0212 mapping

- Source: [Unicode JIS0212.TXT](https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/JIS0212.TXT), table v2.0, header dated 2015-12-02.
- File: `unicode/JIS0212.TXT`, unchanged; SHA-256: `477820bb3055bbcc90880d788cd95607d221dc94457bae249231adecf13c12e6`.
- License: [Unicode License v3](unicode/LICENSE.txt), retrieved from https://www.unicode.org/license.txt on 2026-10-06. Original mapping copyright: 2015 Unicode, Inc.
- `python scripts/generate-jis0212.py` verifies this checksum and generates `src/dictionary/jis0212_table.h`. Compilation uses the checked-in table without running the generator or accessing the network. The offline evaluation script (`scripts/evaluate.py`) requires Python 3.11 or later.
- The table supplies all 6,067 assigned SS3 cells; unassigned cells are errors. It follows this Unicode mapping, including JIS `0x2237` to `U+007E` (ASCII tilde), rather than the browser-oriented WHATWG variant's `U+FF5E`.
- The dictionary test reads the separate reference file and checks all assigned cells through the file loader. Existing JIS X 0208 / halfwidth-kana mappings remain Windows CP20932.
- Include `unicode/LICENSE.txt` with binary distributions containing this table.

## skk-zenz

- Source: https://github.com/nekomimist/skk-zenz/tree/db1c923223a94c6406fd19e4d75ef3b54bdf9300
- Copied: `model.cpp`, `model.h`, `prompt.cpp`, `prompt.h`, and their tests under `skk-zenz/`.
- License: [MIT](skk-zenz/LICENSE), Copyright (c) 2026 Hiroyuki Ishikura.
- Source archive SHA-256: `434c090f47934fecdb54304a871e17f7fed423d65a1c5138b1754b08402b0c7c`.
- Local changes: model abort callback, cancellation checks in generation/scoring and before decode; use the default mapped CPU model loading for compatibility with both llama.cpp APIs. Prompt and upstream tests are unchanged.
- The original protocol implementation is not copied. Our bounded protocol supports its `convert` and `score` concepts, with the differences documented in [ARCHITECTURE.md](../docs/ARCHITECTURE.md#推論).

## azooKey llama.cpp (optional CPU backend)

- Source: https://github.com/azooKey/llama.cpp/tree/66afb885a8ddcc0511798aec9fb5a01017941018
- This is the skk-zenz submodule revision at the commit above, on `azookey/b9637-compat`.
- License: [MIT](llama.LICENSE), Copyright (c) 2023–2026 The ggml authors.
- Archive SHA-256: `df8e483c6bb30e358389f6be382a63f14bd8648d327b4994fc2f0b1df63ff97b`.
- `scripts/prepare-zenz.ps1` downloads and verifies the archive into ignored `out/deps/`. Builds themselves do not download anything.
- Sources are unchanged. Parent CMake selects C++17 for llama.cpp and C++20 for our code, and disables GPU/OpenMP/native CPU detection. llama.cpp is linked only into the optional server and model tests.

## upstream llama.cpp experiment

The opt-in `upstream-v0.6.0` experiment uses ggml-org/llama.cpp commit
`8345f333951c661d166b00e6f9362e553768f292` (v0.6.0), archive SHA-256
`ea153f18923e18f47d8c52c55cde0fbc446102cccfa83356964fe7620b7c8898`.
`patches/llama-v0.6.0-zenz.patch` ports the Japanese tokenizer changes from the
azooKey commit above; its parent `aedb2a5e9ca3d4064148bbb919e0ddc0c1b70ab3`
is an upstream commit. The unrelated Metal environment switch is omitted.
The experiment is not accepted for distribution; see [ARM64.md](../docs/ARM64.md).

## zenz model (not bundled)

- Repository: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/tree/c67e03e07d215c869f591b274c1631170d3e11fe
- File: `ggml-model-Q5_K_M.gguf`.
- SHA-256: `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`.
- The pinned model card declares Apache-2.0. The preparation script downloads the card alongside the model into ignored `out/models/`.
- Neither model weights nor a model distribution package are included in this repository. Distribution must include the applicable model notices and license.
