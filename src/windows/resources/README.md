# Application icon

`nskkzime.png` is the approved source image, generated with the built-in ImageGen
tool on 2026-10-12. The design is a dark seated cat with white edging, white facial
features, and white `SKK` lettering on a transparent background.
The final edit prompt is recorded in `generation-prompt.txt`; the edit used the
previous cat concept as its reference image with `transparent_background=true`.

`nskkzime.ico` contains 16, 20, 24, 32, 40, 48, 64, 128, and 256 px images. It is
embedded as the first icon group in the x64/x86 TSF DLL and the settings executable.
The TSF profile uses DLL icon index 0. The settings dialog loads small and large
icons for its title bar and taskbar through `LoadIconMetric`.

To regenerate the ICO after updating the approved PNG, use Python with Pillow:

```powershell
python scripts/build-icon.py
```

The generated ICO is checked in; normal CMake builds do not require Python or
Pillow. The input-mode symbols (`あ`, `ア`, `A`) continue to indicate the active mode.
