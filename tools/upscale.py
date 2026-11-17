"""Batch-upscale exported pictures with OpenModelDB models (for texture packs).

Runs a chain of models (e.g. a 1x de-dither model, then a 4x upscaler) over
every image in a folder. .pth/.safetensors models are loaded with spandrel
(the library chaiNNer uses) and converted once to ONNX next to the model;
inference runs through ONNX Runtime with DirectML (any DirectX 12 GPU: AMD,
NVIDIA, Intel), falling back to the CPU.

Needs: torch (CPU build is enough), spandrel, onnx, onnxruntime-directml
(or onnxruntime), Pillow, numpy. See docs/texture-pack.md.

    python tools/upscale.py IN_DIR OUT_DIR MODEL [MODEL ...] [--limit N] [--only HASH ...]

Output files keep the input's name, so tools/texturepack.py build can match
them by hash. Existing outputs are skipped (resume after an interruption).
"""
from __future__ import annotations

import argparse
import os
import sys
import time

import numpy as np
from PIL import Image


def to_onnx(model_path: str) -> str:
    if model_path.lower().endswith(".onnx"):
        return model_path
    out = os.path.splitext(model_path)[0] + ".onnx"
    if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(model_path):
        return out
    import torch
    from spandrel import ModelLoader
    desc = ModelLoader().load_from_file(model_path)
    print(f"converting {os.path.basename(model_path)} ({desc.architecture.name}, {desc.scale}x) to ONNX")
    net = desc.model.eval()
    dummy = torch.rand(1, 3, 64, 64)
    torch.onnx.export(net, dummy, out, opset_version=17, input_names=["input"], output_names=["output"],
                      dynamic_axes={"input": {2: "h", 3: "w"}, "output": {2: "h", 3: "w"}}, dynamo=False)
    return out


def session(path: str):
    import onnxruntime as ort
    providers = [p for p in ("DmlExecutionProvider", "CPUExecutionProvider") if p in ort.get_available_providers()]
    opts = ort.SessionOptions()
    opts.log_severity_level = 3
    s = ort.InferenceSession(path, opts, providers=providers)
    return s


def run(sessions, img: Image.Image) -> Image.Image:
    x = np.asarray(img.convert("RGB"), dtype=np.float32).transpose(2, 0, 1)[None] / 255.0
    for s in sessions:
        x = s.run(None, {s.get_inputs()[0].name: x})[0]
        x = np.clip(x, 0.0, 1.0).astype(np.float32)
    y = (x[0].transpose(1, 2, 0) * 255.0 + 0.5).astype(np.uint8)
    return Image.fromarray(y, "RGB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("in_dir")
    ap.add_argument("out_dir")
    ap.add_argument("models", nargs="+", help="applied in order")
    ap.add_argument("--limit", type=int, default=0, help="only the first N images")
    ap.add_argument("--only", nargs="*", default=[], help="only images whose name starts with these")
    args = ap.parse_args()

    sessions = [session(to_onnx(m)) for m in args.models]
    print("running on", sessions[0].get_providers()[0])
    files = sorted(f for f in os.listdir(args.in_dir) if f.lower().endswith((".png", ".bmp", ".jpg")))
    if args.only:
        files = [f for f in files if any(f.startswith(o) for o in args.only)]
    if args.limit:
        files = files[:args.limit]
    os.makedirs(args.out_dir, exist_ok=True)
    t0, done = time.time(), 0
    for i, f in enumerate(files):
        out = os.path.join(args.out_dir, os.path.splitext(f)[0] + ".png")
        if os.path.exists(out):
            continue
        run(sessions, Image.open(os.path.join(args.in_dir, f))).save(out + ".tmp", "PNG", compress_level=1)
        os.replace(out + ".tmp", out)
        done += 1
        if done % 25 == 0 or i == len(files) - 1:
            el = time.time() - t0
            print(f"{i + 1}/{len(files)}  {el / done:.2f} s/image  ~{el / done * (len(files) - i - 1) / 60:.0f} min left",
                  flush=True)
    print(f"{done} images -> {args.out_dir} in {time.time() - t0:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
