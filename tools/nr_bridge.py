"""Binary bridge between BetterRemotePlay and the NeuralScreen NGX NR worker.

stdin:  raw RGBA8 frames (width*height*4)
stdout: "READY\\n" once, then raw RGBA8 NR frames (same size)
argv:   --root <neuralscreen folder> [--style N] [--tone 0..2]
        [--structure 0..2] [--skin 0..2] [--mask 0/1] [--ui 0/1]
        [--intensity 0..2]

The bridge owns the worker process, its shared memory and the NR settings; the
C++ app only copies pixels in and out. This reuses the vendor NGX NR pipeline
(multi-pass capable) instead of a single-pass NGX feature.
"""

import argparse
import contextlib
import os
import sys

os.environ["PYGAME_HIDE_SUPPORT_PROMPT"] = "1"
_real_stdout = sys.stdout
sys.stdout = sys.stderr

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("--root", required=True)
parser.add_argument("--style", type=int, default=2)
parser.add_argument("--tone", type=float, default=1.0)
parser.add_argument("--structure", type=float, default=0.5)
parser.add_argument("--skin", type=float, default=0.75)
parser.add_argument("--mask", type=int, default=0)
parser.add_argument("--ui", type=int, default=0)
parser.add_argument("--intensity", type=float, default=1.0)
parser.add_argument("--width", type=int, default=1920)
parser.add_argument("--height", type=int, default=1080)
args = parser.parse_args()

sys.path.insert(0, os.path.join(args.root, "app"))

from pipeline import shutdown_worker, start_worker  # noqa: E402
from protocol import SharedFrameBuffer, send_frame, send_out  # noqa: E402

W, H = args.width, args.height
params = {
    "style": args.style,
    "auto_mask": args.mask,
    "intensity": args.intensity,
    "local_tone": args.tone,
    "local_structure": args.structure,
    "skin_structure": args.skin,
}

shm = SharedFrameBuffer(W, H)
with contextlib.redirect_stdout(sys.stderr):
    worker, logs, reader, stop = start_worker(params, W, H, 0, W, H, shm)
    shm.open_out(W, H)
    send_out(worker, W, H, shm.out_name)
    reader.wait_oak(timeout=20.0)

try:
    sys.stdout = _real_stdout
    motion = np.zeros((H, W, 2), np.float16)
    sys.stdout.write("READY\n")
    sys.stdout.flush()
    out = sys.stdout.buffer
    inp = sys.stdin.buffer
    index = 0
    while True:
        data = inp.read(W * H * 4)
        if len(data) < W * H * 4:
            break
        rgba = np.frombuffer(data, dtype=np.uint8).reshape(H, W, 4)
        send_frame(worker, index, rgba, motion, reset=(index == 0), pts=index, shm=shm,
                   want_pixels=True)
        pixels = reader.recv(index, timeout=30.0)
        if pixels is None:
            pixels = rgba
        out.write(np.ascontiguousarray(pixels).tobytes())
        out.flush()
        index += 1
finally:
    with contextlib.redirect_stdout(sys.stderr):
        shutdown_worker(worker, stop)
        shm.close()
