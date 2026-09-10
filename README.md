# AERA Recorder

Signed runtime plugin for AERA Recovery Project's native screen recorder.

The recovery host owns display capture and passes bounded RGBA frames over a
sealed shared-memory channel. This plugin contains only the isolated GStreamer
worker and its OpenH264/MP4 runtime. Recordings are written to
`/sdcard/AERA/Recordings`.

## Build

```sh
./source/build-runtime.sh stage
./source/pack.py stage build
```

