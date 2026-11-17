# HD texture packs

A texture pack gives the game sharper, upscaled versions of its pictures. You make it yourself from your own game files with an AI upscaler. The pack is derived from the copyrighted game art, so keep it for yourself like the game files (`texturepack/` is gitignored).

## How it works

The original game never sees the HD pictures. It keeps loading, measuring and hit-testing its small 8-bit bitmaps exactly as before:
- sprites build their masks pixel by pixel;
- buttons test clicks with `GetPixel`.

When the game creates a bitmap (`CreateDIBitmap`, `LoadBitmap`), the runtime hashes the decoded pixels. If the pack has a picture with that hash, the runtime attaches it as an **HD layer** (`HdLayer` in `src/recomp/win16/gdi.hpp`).

`blit()` carries HD layers along. A copy from a bitmap with a layer also draws into the destination's layer. Off-screen buffers get a layer the first time an HD picture is copied into them.

Everything else changes only the small pixels:
- text, lines and fills;
- the game's own sprite masks;
- videos.

Before each frame is shown, `hd_sync()` enlarges whatever changed. The screen's HD layer (640×480 × scale) is what the window shows, and F12 screenshots save it at full size.

Code: `src/recomp/enhance/texture_pack.cpp` (finding the pack, the hash, PNG loading with a cache), `gdi.cpp` (`hd_*`, `blit`), and `display.cpp` (`present`).

## Making a pack

You need Python 3.10+ with Pillow.

1. **Export** every picture once, named by its hash. This gives about 1,880 unique pictures, about 170 MB.

   ```bash
   python -I tools/texturepack.py export GAME_DIR work
   ```

   `GAME_DIR` is the folder that holds `MYULK`, `PICTDLL` and `ART`. That is either `%APPDATA%\myulk-recomp\Myulk\game` after the first-start import, or `extracted/iso` in the repository. The pictures land in three folders, because each suits a different model:

   | Folder | What | Count |
   |---|---|---|
   | `work/src/scenes` | 450×250 adventure frames and `IMAGES\` | ~1,330 |
   | `work/src/art` | farm, animals, buildings (`ART\`) | ~360 |
   | `work/src/ui` | buttons, form graphics, icons | ~190 |

2. **Upscale** each folder with any tool that keeps the file name's leading 16 hex digits (added suffixes are fine). Recommended tools:
   - **chaiNNer** with models from OpenModelDB. Chain a 1× de-dither model (e.g. `1x_DeDither`) before a 4× model (`4x-UltraSharp`, `4x_foolhardy_Remacri`). The 1996 art is 256-colour error-diffused, and upscaling the dither as-is magnifies the speckle.
   - **Upscayl** (Real-ESRGAN): the simplest. Use batch mode on each folder.
   - **realesrgan-ncnn-vulkan** for scripting.

   - **`tools/upscale.py`** (in this repository) runs OpenModelDB models in batch without a GUI. It loads `.pth` models with spandrel (the library chaiNNer uses) and converts them once to ONNX. It then runs them on the GPU through ONNX Runtime + DirectML, which works on AMD, NVIDIA and Intel GPUs under Windows. Models are applied in the order given, and finished pictures are skipped, so an interrupted run can simply be restarted.

     ```bash
     python -m venv upscale-venv
     upscale-venv/Scripts/python -m pip install torch --index-url https://download.pytorch.org/whl/cpu
     upscale-venv/Scripts/python -m pip install spandrel onnx onnxruntime-directml pillow numpy
     upscale-venv/Scripts/python tools/upscale.py work/src/scenes upscaled/scenes models/4x-FSDedither.pth
     ```

     A trial on scenes and ART pictures compared [4x-FSDedither](https://openmodeldb.info/models/4x-FSDedither) on its own (GPL-3.0) with [1x-DitherDeleterV3-Smooth](https://openmodeldb.info/models/1x-DitherDeleterV3-Smooth) followed by [4x-UltraSharp](https://openmodeldb.info/models/4x-UltraSharp):
     - **FSDedither** keeps the original's shapes and textures, with some grain left where the dithering was heavy.
     - **DitherDeleterV3 + UltraSharp** is cleaner but painterly, and reinvents surfaces (brick turns into planks).

     FSDedither is the recommended choice. On an RX 7900 XT it takes about 5 s per picture, about 2.5 hours for everything.

   Avoid diffusion/"creative" upscalers. Most scene frames are animation sequences, and these tools invent different detail in each frame, so the animation flickers.

3. **Build** the pack. `--scale` is 2, 3 (default; a 1920×1440 picture) or 4. Every picture is resized to exactly scale × its original size, so the upscaler's own factor does not matter.

   ```bash
   python -I tools/texturepack.py build work texturepack --scale 3 upscaled/scenes upscaled/art upscaled/ui
   ```

   Pictures you leave out simply stay as they are, so you can start with only the scenes.

4. **Put the pack where the game finds it**. The first location that has `pack.json` is used:
   - `MYULK_TEXTURE_PACK`
   - `texturepack/` next to the program
   - `%APPDATA%\myulk-recomp\Myulk\texturepack`
   - `texturepack/` in, or next to, the game folder (in the repository: `extracted/texturepack`)

   The log says `texture pack …: N pictures at 3x`.

**F6**, or *Texturpaket* in the settings menu (F2), switches between the HD pictures and the originals while you play. The switch is instant. The HD layers are kept up to date even while the originals are shown, and the choice is saved (`texture_pack` in `settings.txt`). With the originals shown, *Avdithring* works as usual.

## Testing

- `python -I tools/texturepack.py build work texturepack-test --scale 3 --fake` makes a pack of plain enlarged pictures with a green tint. Every replaced picture shows up green in the game.
- `MYULK_TEXTURE_PACK_LOG=1` logs every bitmap the game creates, with its hash and `HD` or `-`.
- `MYULK_HD_DEBUG=1` logs larger blits and whether source and destination have HD layers.
- `python -I tools/texturepack.py hash FILE.bmp` prints the hash the game computes for a picture.

## Limits (for now)

- **Sprites** (TSprite) build their image and mask with `SetPixel`. Animated sprites over an HD background are drawn at the original resolution (enlarged), while the background around them stays HD.
- **Videos** are enlarged plainly. Upscaled videos would need their own replacement files per AVI.
- **Desktop only.** The browser build does not load packs yet.
- A pack picture with the wrong size is skipped (logged). Rebuild the pack after changing `--scale`.
