#!/usr/bin/env python3
"""Export the manga-image-translator DBNet detector (ResNet34 + DB head) to ONNX.

The exported model is the same one yakuyomi-engine runs through NCNN
(detect-20241225.ckpt, m-i-t beta-0.3 release), but as a portable ONNX graph
so lp2psd can run it with the onnxruntime.dll it already loads.

Interface (mirrors yakuyomi-engine's Detector.kt / ncnn_jni.cpp):
  in0  [1,3,H,W]  RGB, NCHW, normalized (x/127.5 - 1)
                  (preprocessing: long side resized to `size`, padded right/
                   bottom to a multiple of 256, pad area = black)
  out0 [1,2,H,W]  db (full resolution): ch0 = shrink_map RAW LOGITS (not
                  sigmoid), ch1 = threshold_map (sigmoid). The engine applies
                  sigmoid to ch0 itself (m-i-t does db.sigmoid() outside the
                  model). Changing this breaks every box.
  out1 [1,1,H/2,W/2] mask: per-pixel text-stroke mask (already sigmoid).

Usage:
  python scripts/export_dbnet_onnx.py [output.onnx]

Env overrides:
  MIT_CLONE   manga-image-translator clone dir (default third_party/...)
  DET_CKPT    path to detect-20241225.ckpt (default third_party/ckpt/...,
              auto-downloaded + sha256-verified when missing)
"""

import argparse
import hashlib
import importlib.util
import os
import subprocess
import sys
import urllib.request

import numpy as np
import torch

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIT_CLONE = os.environ.get(
    "MIT_CLONE", os.path.join(REPO, "third_party", "manga-image-translator"))
CKPT = os.environ.get(
    "DET_CKPT", os.path.join(REPO, "third_party", "ckpt", "detect-20241225.ckpt"))

CKPT_URL = ("https://github.com/zyddnys/manga-image-translator/releases/"
            "download/beta-0.3/detect-20241225.ckpt")
CKPT_SHA256 = ("67ce1c4ed4793860f038c71189ba9630a7756f7683b1ee5afb69ca0687dc502e")

# Trace shape mirrors what the engine actually runs: dbnetInputSize=1024,
# e.g. a 1351x1920 page -> resize_aspect to 721x1024 -> pad to 768x1024.
# Dynamic axes make the exported graph shape-agnostic anyway; this shape is
# only used for tracing.
TRACE_H, TRACE_W = 1024, 768


def sha256_of(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def fetch_ckpt():
    if os.path.exists(CKPT) and sha256_of(CKPT) == CKPT_SHA256:
        print(f"ckpt already present and verified: {CKPT}")
        return
    os.makedirs(os.path.dirname(CKPT), exist_ok=True)
    tmp = CKPT + ".part"
    print(f"downloading {CKPT_URL} ...")
    urllib.request.urlretrieve(CKPT_URL, tmp)
    got = sha256_of(tmp)
    if got != CKPT_SHA256:
        raise SystemExit(f"sha256 mismatch for {CKPT}: {got}")
    os.replace(tmp, CKPT)
    print(f"ckpt ready: {CKPT}")


def _pkg_shell(name, path):
    """Put a package module into sys.modules WITHOUT executing __init__.py.

    manga_translator/__init__.py drags in heavy deps (translators -> openai),
    and detection/__init__.py imports paddle_rust. We only need the pure-torch
    DBNet module, so stub the package shells and let relative imports resolve
    through __path__.
    """
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(path, "__init__.py"), submodule_search_locations=[path])
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    return mod


def load_text_detection():
    if not os.path.isdir(MIT_CLONE):
        print(f"cloning manga-image-translator into {MIT_CLONE} ...")
        subprocess.check_call([
            "git", "clone", "--depth", "1",
            "https://github.com/zyddnys/manga-image-translator.git", MIT_CLONE])
    base = os.path.join(MIT_CLONE, "manga_translator")
    for pkg in ("manga_translator",
                "manga_translator.detection",
                "manga_translator.detection.default_utils"):
        rel = pkg[len("manga_translator"):].strip(".")
        _pkg_shell(pkg, os.path.join(base, *rel.split(".")) if rel else base)
    name = "manga_translator.detection.default_utils.DBNet_resnet34"
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(base, "detection", "default_utils", "DBNet_resnet34.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod.TextDetection


def build_model():
    TextDetection = load_text_detection()
    model = TextDetection()
    sd = torch.load(CKPT, map_location="cpu")
    sd = sd["model"] if "model" in sd else sd
    model.load_state_dict(sd)
    model.eval()
    return model


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", nargs="?", default=os.path.join(REPO, "dbnet_detect.onnx"))
    ap.add_argument("--skip-verify", action="store_true")
    args = ap.parse_args()

    fetch_ckpt()
    model = build_model()

    dummy = torch.zeros(1, 3, TRACE_H, TRACE_W)
    with torch.no_grad():
        db, mask = model(dummy)
    print(f"torch forward ok: in {tuple(dummy.shape)} -> db {tuple(db.shape)} "
          f"mask {tuple(mask.shape)}")
    assert db.shape[1] == 2, f"db should be 2ch (eval branch), got {db.shape[1]}"

    torch.onnx.export(
        model,
        dummy,
        args.out,
        export_params=True,
        opset_version=17,
        do_constant_folding=True,
        input_names=["in0"],
        output_names=["out0", "out1"],
        dynamo=False,  # legacy TorchScript exporter: robust for this conv net
        dynamic_axes={
            "in0": {0: "batch", 2: "height", 3: "width"},
            "out0": {0: "batch", 2: "height", 3: "width"},
            "out1": {0: "batch", 2: "height", 3: "width"},
        },
    )
    print(f"onnx exported: {args.out} ({os.path.getsize(args.out):,} B)")
    if not args.skip_verify:
        verify(args.out, model)


def verify(path, model):
    try:
        import onnxruntime as ort
    except ImportError:
        print("onnxruntime not installed; skipping numerical verification")
        return
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    x = np.random.RandomState(0).randn(1, 3, TRACE_H, TRACE_W).astype(np.float32)
    db_o, mask_o = sess.run(None, {"in0": x})
    with torch.no_grad():
        db_t, mask_t = model(torch.from_numpy(x))
    db_t, mask_t = db_t.numpy(), mask_t.numpy()
    p_o = 1 / (1 + np.exp(-db_o[:, 0]))
    p_t = 1 / (1 + np.exp(-db_t[:, 0]))
    print(f"vs torch eager: sigmoid(ch0) maxdiff={np.abs(p_o - p_t).max():.6f} "
          f"ch1 maxdiff={np.abs(db_o[:, 1] - db_t[:, 1]).max():.6f} "
          f"mask maxdiff={np.abs(mask_o - mask_t).max():.6f}")


if __name__ == "__main__":
    main()
