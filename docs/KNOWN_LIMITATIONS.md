# Known limitations - 1.0

## Inputs that do not exist in a video stream

DLSS Frame Generation is an engine feature. A Remote Play frame is a finished
2D image: no depth buffer, no game motion vectors, no jitter or camera
matrices. The closed NeuralScreen worker feeds the DLSS-G runtime **flat depth
and optical-flow-estimated motion**. Generated frames therefore are not
bit-identical to a game's native DLSS-G and can show warping, HUD edges and
disocclusion artifacts. The application labels this as the experimental
flat-depth path.

## Frame generation rates

- DLSS-G 2x is the clean cadence.
- 3x-6x require the user-supplied SM86 proxy; at 60 fps input the worker lands
  near 150-165 generated fps, between display refreshes.
- 4x from a 30 fps source asks for 120 but measured ~94 fps with late generated
  frames in this environment. It is not offered as the smooth path.
- RIFE 4.25 Lite performs one midpoint per pair and is only used below 45 fps.
  It cannot generate two frames per source at 60 fps within the latency budget.

## Remote Play protocol

- The console's encoder exposes 1920x1080 HEVC. 1440p/2160p are client-side
  processing targets, not stream resolutions.
- Codec selection, HDR streaming and console-side bitrate behaviour are
  controlled by the console. The app sets the requested bitrate and fps only.
- PS5 sessions may remain occupied for ~30 s after a forced process kill.

## Presentation

- Borderless fullscreen is a DWM-composited window, not exclusive fullscreen.
  Exclusive mode cannot host the DLSS-G worker window above the video.
- HDR output, monitor mode switching and per-adapter selection are not part of
  1.0.
- V-Sync is applied to the parent presentation. DLSS-G's own presenter is paced
  by its compositor (latency 1), which is not configurable from this client.

## Validation not possible here

- True 4K/60 game validation: the reference display is 2560x1440 and the game
  content used for testing is 30 fps.
- Multi-hour soak, RTX 5080 second-machine pass, HDR display, and secondary
  monitor fullscreen were not available in this session.
- 120 fps output was verified on the 1080p worker path; 1440p DLSS-G is limited
  by the worker to about 100 generated fps.
