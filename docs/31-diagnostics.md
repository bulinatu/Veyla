# 31 - Diagnostics (testing.log)

Status: **0.4.3** (19 Nov 2026).

The app writes `%APPDATA%\BetterRemotePlay\testing.log`. Send that one file when
something stutters on another PC; it is enough to tell pacing from inference.

## Header (once per launch)

```
local=2026-11-19 15:39:37
os=Windows 10 Pro build 26300
cpu=AMD Ryzen 9 9950X3D 16-Core Processor           cores=32
gpu=NVIDIA GeForce RTX 4090 vram=24138MB
display=240Hz window=1600x900 fullscreen=0 hdr=0
profile: engine=RIFE target=120 sr=1 mode=5 fg=2 nr=0 fps=30 bitrate=100000 ...
rife: runtime=<TensorRT folder> engine_bytes=13140900
```

## One line per second

```
t=31.1 !! fps=30.0 src=11.1 content=18.9 rife=20.0/21.72 cad=49.47/658.41 gen=23 q=0 fg=0.0 late=0 stalls=0 miss=0 dec=7.99/10.16 drops=0 lost=0 buf=1 aud=0
```

| Field | Meaning |
| --- | --- |
| `!!` | At least one anomaly in this second: late ticks, a cadence frame over 30 ms, more than 3 present misses while streaming, lost frames or audio underruns. |
| `fps` | Frames the streaming layer produced. |
| `src` | Source frames pulled by the app. |
| `content` | Unique frames after duplicate skipping (30 on a 30 fps game). |
| `rife=<shown>/<ms>` | RIFE output rate and inference time. `0/0` means RIFE was not active. |
| `cad=<avg>/<max>` | Interval between frames handed to the presenter. A steady 16.7 ms is 60 fps. |
| `gen` | Midpoints generated in that second. |
| `q` | Generated frames waiting to be presented. |
| `fg` | NVIDIA worker displayed rate (0 when RIFE runs). |
| `late`/`stalls` | Ticks over 1.5x / 2.5x the refresh budget. |
| `miss` | Refreshes that passed without a new present, in that second. |
| `dec` | Decode time average / peak. |
| `drops`/`lost` | Jitter-buffer drops / RTP losses. |
| `buf` | Jitter buffer target. |
| `aud` | Audio underruns. |

The file rotates at 4 MB to `testing.1.log`. `Settings > About > Open
diagnostics folder` opens the folder.

## Reading a report

- `cad` around 16.7 with occasional 33: a generated frame was late; check
  `rife=` (if it spikes near the interval, the GPU is the limit).
- `content` near 60 on a 30 fps game: duplicate skipping failed; the game is
  being shown as 60 with repeated frames.
- `late` and `miss` together: presentation/DWM, not the neural chain.
- `fg` non-zero instead of `rife`: NVIDIA frame generation was active, not RIFE
  (usually a foreign engine that could not load).
- `dec` high with `drops`: decode/jitter, not frame generation.

## First real report (RTX 5080, 0.4.3)

Header: 5080, 240 Hz, `engine=RIFE`, `fps=30`, `buf=2`, engine 39.5 MB (rebuilt
for sm120 - the setup worked). Per-second lines showed:

- `rife=19-25 ms`, so inference itself was fine.
- `gen` fell to 3-14 per second instead of 30, with `rife` shown dropping to
  28-45. Cause: the fast-motion guard (0.4.2) used a threshold of 12 and skipped
  generation during ordinary camera motion. It now defaults to **off**
  (`BRP_RIFE_SAFE_MOTION=1` enables it, threshold 18); the pair-gap guard stays.
- `cad` drifted from 16.7 to 18.6-19.6 ms with 66-100 ms peaks: presentation
  stalls, not inference.
- `aud` underruns grew 6 -> 39, and the `src` metric moved between 18 and 29
  while `content` stayed near 30. `src` comes from a different window than
  `fps`, so it is not a reliable delivery measure; the audio underruns are real
  and point at the audio path or the network, not at frame generation.

0.4.4 adds `skip=gap/motion/pass` per second so the next report says why frames
were not generated, and `fs` for the borderless state.

## 0.4.5 - toggle and the last two fixes

- The log is **off by default**. Settings > Debug > "Testing log (per second)"
  turns it on; turning it off closes the file. The About button still opens the
  folder.
- Cadence is now per second. It used to reset only under `BRP_FG_PROBE`, so the
  average crept up over a session and one stall stayed in `max` forever (the
  3308 ms entry in the 0.4.4 report).
- `ref=` adds the refresh counter delta and `aud=` is now underruns in that
  second, not the session total.
- Presentation hands over **one due frame per tick** instead of the newest of
  several. A stall that makes two frames due no longer drops one.
