`groovie2.exe` is a Windows GUI and command-line research tool for studying,
viewing, extracting, and re-encoding the Clandestiny / CDY Groovie-2 asset
pipeline. The same decoder also builds as CLI-only executables on Linux and
macOS, so non-Windows users can still pass argv-based extraction commands
without any GUI dependency. It is written in C++20, uses the Win32 API for the
Windows graphical asset viewer, Windows Imaging Component for JPEG chunks on
Windows, libjpeg for JPEG chunks on Linux/macOS, and the original Clandestiny
game data files.

This repository is intentionally scoped to Clandestiny. It is not a merged
11th Hour documentation tree. The correct mental model is:

```text
Clandestiny / CDY
    Groovie-2 era runtime
    V32WIN.EXE host executable
    GJD.GJD global pack index
    DIR.RL global resource directory
    MEDIA/*.GJD packed blobs
    GROOVIE/*.GRV scripts
    ROQ/RNR streams with Clandestiny-specific motion variants
```

---

**Quick Navigation:**

- **Want to browse assets?** -> [Running the GUI](#running-the-gui)
- **Need command-line extraction?** -> [Command Line Utilities](#command-line-utilities)
- **Understanding the data formats?** -> [Game Engine Architecture](#game-engine-architecture)
- **Need the Clandestiny-specific warnings?** -> [Known Clandestiny Fuckery](#known-clandestiny-fuckery)
- **Building from source?** -> [Developers](#developers)

---

**Table-of-Contents**
- [Disclaimer](#disclaimer)
- [Scope](#scope)
- [Usage](#usage)
  - [Running the GUI](#running-the-gui)
  - [Configuration](#configuration)
  - [Command Line Utilities](#command-line-utilities)
    - [`-root <DIR>`](#-root-dir)
    - [`-list`](#-list)
    - [`-all`](#-all)
    - [`<RESOURCE_NAME>`](#resource_name)
    - [Direct ROQ/RNR/ROL file](#direct-roqrnrrol-file)
    - [`-raw`](#-raw)
    - [`-out <DIR>`](#-out-dir)
    - [`-alt`](#-alt)
    - [`-norm`](#-norm)
- [Game Engine Architecture](#game-engine-architecture)
  - [On-Disc Layout](#on-disc-layout)
  - [GJD.GJD](#gjdgjd)
  - [DIR.RL](#dirrl)
  - [GJD](#gjd)
  - [GRV](#grv)
  - [ROQ / RNR](#roq--rnr)
    - [Stream Header](#stream-header)
    - [Chunk Header](#chunk-header)
    - [Chunk Types](#chunk-types)
    - [0x1001 Video Info](#0x1001-video-info)
    - [0x1002 Quad Codebook](#0x1002-quad-codebook)
    - [0x1011 Quad Vector Frame](#0x1011-quad-vector-frame)
    - [0x1012 JPEG Still](#0x1012-jpeg-still)
    - [0x1013 Hang](#0x1013-hang)
    - [0x1020 / 0x1021 Audio](#0x1020--0x1021-audio)
    - [0x1030 Packet](#0x1030-packet)
  - [Music and External Audio](#music-and-external-audio)
  - [System Files](#system-files)
- [Known Clandestiny Fuckery](#known-clandestiny-fuckery)
  - [V32WIN.EXE Is Not The Whole Engine Contract](#v32winexe-is-not-the-whole-engine-contract)
  - [Named Motion Variant Streams](#named-motion-variant-streams)
  - [26A_GRAF Overlay Behavior](#26a_graf-overlay-behavior)
  - [Alpha Codebooks](#alpha-codebooks)
  - [Short-Height / Interlaced Streams](#short-height--interlaced-streams)
- [Viewer Architecture](#viewer-architecture)
  - [Asset Table](#asset-table)
  - [Async Decode](#async-decode)
  - [Playback](#playback)
  - [Rendering Modes](#rendering-modes)
  - [MP4 Encoding](#mp4-encoding)
- [Developers](#developers)
  - [Build System Overview](#build-system-overview)
  - [Prerequisites](#prerequisites)
  - [Build Invocation](#build-invocation)
  - [Technical Anchors](#technical-anchors)
- [CHANGELOG](#changelog)
  - [2026-05-22](#2026-05-22)
  - [2026-05-21](#2026-05-21)

# Disclaimer

This project is an academic reverse-engineering and preservation effort focused
on the original software Clandestiny. It is not officially affiliated with,
connected to, or endorsed by Trilobyte, Virgin Interactive, or any current
rights holder. The project is intended for non-profit research, documentation,
format study, and compatibility work. It does not include or replace the
original game data. You must provide your own legally obtained Clandestiny
files.

All trademarks and registered trademarks mentioned here are the property of
their respective owners.

# Scope

This repository documents and implements Clandestiny / CDY only.

Clandestiny belongs to the Groovie-2 lineage: it uses the `GJD.GJD` pack index,
the global `DIR.RL` resource directory, `MEDIA/*.GJD` data packs, `GROOVIE/*.GRV`
scripts, and ROQ/RNR video streams. The Windows disc also ships a `V32WIN.EXE`
runtime, but this project treats that executable as only one clue in a larger
data contract.

The current local Clandestiny data set has:

| Component                 | Observed value |
| ------------------------- | -------------- |
| `GROOVIE/DIR.RL` size      | 134,240 bytes  |
| `DIR.RL` record size       | 32 bytes       |
| `DIR.RL` total records     | 4,195          |
| ROQ/RNR visual resources   | 3,967          |
| `GROOVIE/GJD.GJD` size     | 183 bytes      |
| `GJD.GJD` pack slots       | 13             |
| `GROOVIE/*.GRV` scripts    | 40             |

# Usage

## Running the GUI

Running `groovie2.exe` with no arguments opens the Clandestiny asset viewer:

```cmd
groovie2.exe
```

The GUI expects a Clandestiny disc root, such as:

```text
Clandestiny\disc1
Clandestiny\disc2
```

The root must contain the `GROOVIE` and `MEDIA` directories, or the program must
be pointed at a layout from which those folders can be resolved.

The main window contains:

- A sortable asset table on the left.
- A video/still preview pane on the right.
- A `Play` button for timed in-window playback.
- A `Fit` / `Original` toggle for preview scaling.
- An `Encode MP4` button for ffmpeg-based lossless MP4 output.
- A status bar for decode, playback, and encode state.

The asset table defaults to sorting by `Size` in descending order. Click any
column header to sort by that column.

## Configuration

The GUI stores both configured Clandestiny disc roots in `config.json`:

```json
{
  "clandestiny_disc1_root": "E:\\oezman-11h\\Clandestiny\\disc1",
  "clandestiny_disc2_root": "E:\\oezman-11h\\Clandestiny\\disc2"
}
```

You can change this through:

```text
Edit -> Preferences...
```

Older configs using `clandestiny_root` are still accepted. If the legacy path
points at `disc1` or `disc2`, the loader tries to infer the sibling disc folder
beside it.

If no config path is present, the GUI first tries:

```text
./Clandestiny/disc1
./Clandestiny/disc2
```

and then falls back to the current working directory.

## Command Line Utilities

The executable also works as a command-line ROQ/RNR decoder. On Windows, when
arguments are provided, the GUI is bypassed and a console is attached or
created. On Linux and macOS, the build is CLI-only: it always enters this
argument parser and never links or opens a GUI.

### `-root <DIR>`

Sets the Clandestiny data root used for resource lookup.

```cmd
groovie2.exe -root Clandestiny\disc1 -list
```

The root may be the game root, a disc root, or in some layouts the `GROOVIE`
directory itself. The loader searches for `GJD.GJD` and `DIR.RL`, then resolves
GJD pack names through the matching `MEDIA` path.

### `-list`

Lists every visual ROQ/RNR resource in the catalog:

```cmd
groovie2.exe -root Clandestiny\disc1 -list
```

Example output:

```text
hidswtch.roq  size=28667  gjd=0  disks=0x3
disc1.roq     size=45294  gjd=0  disks=0x3
act_01.rnr    size=68234375  gjd=2  disks=0x1
```

### `-all`

Decodes every visual ROQ/RNR resource in the catalog:

```cmd
groovie2.exe -root Clandestiny\disc1 -all
```

This is intentionally heavy. Clandestiny has thousands of visual resources, and
large actor streams can be tens of megabytes each before extraction.

### `<RESOURCE_NAME>`

Decodes one or more named resources from `DIR.RL`:

```cmd
groovie2.exe -root Clandestiny\disc1 act_23.rnr
```

Each output sequence is written to a folder named after the resource. Frames are
written as PNG by default, and audio chunks are written as `audio.wav` when
present.

### Direct ROQ/RNR/ROL file

Decodes a direct standalone stream file without using `GJD.GJD` or `DIR.RL`:

```cmd
groovie2.exe SCRATCH1.ROL
```

This path is useful for isolated system streams or already-extracted resources.

### `-raw`

Writes raw RGBA8888 frames instead of PNG:

```cmd
groovie2.exe -root Clandestiny\disc1 -raw act_23.rnr
```

Each frame is written as a row-major top-to-bottom `.raw` file.

### `-out <DIR>`

Sets the output directory:

```cmd
groovie2.exe -root Clandestiny\disc1 -out extracted act_23.rnr
```

### `-alt`

Forces the alternate Clandestiny motion decoder:

```cmd
groovie2.exe -root Clandestiny\disc1 -alt act_23.rnr
```

This is normally selected automatically for known Clandestiny variant streams.

### `-norm`

Forces the normal motion decoder:

```cmd
groovie2.exe -root Clandestiny\disc1 -norm passage1.roq
```

This is mainly useful for comparing motion-block behavior during decoder
research.

# Game Engine Architecture

Clandestiny uses a Groovie-2 resource architecture with a global directory and
multiple media packs. The important shift from the earlier Groovie-1 / The 7th
Guest style is that resources are no longer discovered through small per-pack
RL files. Clandestiny uses one global `DIR.RL` table and one global `GJD.GJD`
pack-name table.

## On-Disc Layout

The observed disc layout is:

```text
Clandestiny/
    disc1/
        GROOVIE/
            GJD.GJD
            DIR.RL
            CLANMAIN.GRV
            26A_GRAF.GRV
            ...
        MEDIA/
            CDNAV.GJD
            CDPUZ.GJD
            CDANIM.GJD
            CDDEVICE.GJD
            ...
        SYSTEM/
            V32WIN.EXE
            SCRATCH1.ROL
            LOAD.RAW
            COVER.RAW
            ...
    disc2/
        GROOVIE/
            GJD.GJD
            DIR.RL
            CLANMAIN.GRV
            ...
        MEDIA/
            CDNAV.GJD
            CDPUZ2.GJD
            CDANIM3.GJD
            CDANIM3B.GJD
            ...
```

Disc 1 and disc 2 share the same `GROOVIE/GJD.GJD` and `GROOVIE/DIR.RL` shape,
but the actual media packs present under `MEDIA` differ by disc.

## GJD.GJD

`GJD.GJD` is a text index mapping GJD pack filenames to numeric pack IDs. The
observed Clandestiny index is:

```text
cdnav.gjd 0
cdpuz.gjd 1
cdanim.gjd 2
cdpuz2.gjd 3
cdpuz2a.gjd 4
cddevice.gjd 5
cddev2.gjd 6
cdanim2.gjd 7
cdanim2a.gjd 8
cdanim3.gjd 9
cdanim3a.gjd 10
cdanim3b.gjd 11
zfatrols.gjd 12
```

The resource loader reads this table first. A `DIR.RL` record then uses its
`gjd` field as an index into this list.

## DIR.RL

`DIR.RL` is the global Clandestiny resource directory. It is a flat array of
32-byte little-endian records:

| Offset | Type     | Name     | Description                                      |
| ------ | -------- | -------- | ------------------------------------------------ |
| 0      | uint32   | disks    | Disc availability bitmask                        |
| 4      | uint32   | offset   | Byte offset inside the selected GJD pack         |
| 8      | uint32   | size     | Resource byte length                             |
| 12     | uint16   | gjd      | Index into `GJD.GJD`                             |
| 14     | char[18] | name     | Null-padded resource name                        |

The current `DIR.RL` file is 134,240 bytes. Dividing by 32 gives 4,195 resource
records.

Example decoded rows:

| Name         | Size     | GJD | Disks |
| ------------ | -------- | --- | ----- |
| `gblack.roq` | 5,459    | 0   | 0x3   |
| `disc1.roq` | 45,294   | 0   | 0x3   |
| `act_23.rnr`| 21,696,504 | 10 | 0x2   |
| `act_01.rnr`| 68,234,375 | 2  | 0x1   |

The `disks` field is not just decoration. It tells the runtime which disc or
disc set can satisfy a resource request.

## GJD

The `.GJD` files in `MEDIA` are packed data blobs. They do not need an internal
file table for this workflow because `DIR.RL` is the authoritative external
directory.

To read a resource:

```text
packName = gjdIndex[dirRecord.gjd]
seek MEDIA/packName to dirRecord.offset
read dirRecord.size bytes
decode bytes according to dirRecord.name / stream signature
```

Disc 1 contains packs such as:

```text
CDNAV.GJD
CDPUZ.GJD
CDANIM.GJD
CDDEVICE.GJD
CDDEV2.GJD
CDANIM2.GJD
CDANIM2A.GJD
ZFATROLS.GJD
```

Disc 2 contains packs such as:

```text
CDNAV.GJD
CDPUZ2.GJD
CDPUZ2A.GJD
CDDEVICE.GJD
CDDEV2.GJD
CDANIM3.GJD
CDANIM3A.GJD
CDANIM3B.GJD
ZFATROLS.GJD
```

## GRV

`.GRV` files are Groovie scripts. Clandestiny's main script is:

```text
CLANMAIN.GRV
```

The local script inventory includes puzzle and room-specific scripts such as:

```text
02_RACK.GRV
03_LOCK.GRV
04_BABEL.GRV
14_KNITS.GRV
16_CUBES.GRV
17_COOK.GRV
23_FROGS.GRV
24_13PUZ.GRV
26A_GRAF.GRV
26B_CID.GRV
CHEAT.GRV
CH_TIPS.GRV
SAVE_CAM.GRV
```

The current tool does not attempt to run the full GRV VM. It focuses on catalog
loading, ROQ/RNR decoding, playback, and export. GRV disassembly and VM work
should be treated as a separate layer above the resource and video systems.

## ROQ / RNR

Clandestiny visual resources are mostly `.roq` and `.rnr` streams. The current
decoder treats both as ROQ-family streams with the same chunk grammar.

### Stream Header

The stream begins with an 8-byte chunk-like header:

| Offset | Type   | Name     | Description                                      |
| ------ | ------ | -------- | ------------------------------------------------ |
| 0      | uint16 | type     | Signature chunk, expected `0x1084`               |
| 2      | uint32 | size     | Usually 0 for signature                          |
| 6      | uint16 | argument | Frame rate, with 0 treated as 30 fps             |

If the signature chunk has `size == 0` and `argument == 0`, the decoder uses
30 fps. Otherwise, a zero argument still falls back to 30 fps.

### Chunk Header

Every following chunk uses the same 8-byte header:

| Offset | Type   | Name     | Description                                      |
| ------ | ------ | -------- | ------------------------------------------------ |
| 0      | uint16 | type     | Chunk type                                       |
| 2      | uint32 | size     | Payload size in bytes                            |
| 6      | uint16 | argument | Chunk-specific parameter                         |

All integer fields are little-endian.

### Chunk Types

| Type     | Name              | Purpose                                      |
| -------- | ----------------- | -------------------------------------------- |
| `0x1000` | Quad              | Container/quad marker skipped by this tool   |
| `0x1001` | Video info        | Dimensions, alpha flag, info constants       |
| `0x1002` | Quad codebook     | VQ codebook update                           |
| `0x1011` | Quad vector frame | Motion/vector-quantized video frame          |
| `0x1012` | JPEG still        | Full-frame JPEG still                        |
| `0x1013` | Hang              | Duplicate/hold current frame                 |
| `0x1020` | Mono sound        | Delta-coded sound chunk                      |
| `0x1021` | Stereo sound      | Delta-coded sound chunk                      |
| `0x1030` | Packet            | Packet/container chunk, skipped currently    |

### 0x1001 Video Info

The video info payload is 8 bytes:

| Offset | Type   | Name     | Description                         |
| ------ | ------ | -------- | ----------------------------------- |
| 0      | uint16 | width    | Frame width                         |
| 2      | uint16 | height   | Frame height                        |
| 4      | uint16 | unknown1 | Expected `8` in normal streams      |
| 6      | uint16 | unknown2 | Expected `4` in normal streams      |

The chunk argument is used as the alpha-codebook flag. Current accepted values
are:

| Argument | Meaning                         |
| -------- | ------------------------------- |
| 0        | Normal opaque codebook vectors  |
| 1        | Codebook vectors include alpha  |

The decoder also marks very short-height streams as interlaced-style sources
when:

```text
height <= width / 3
```

### 0x1002 Quad Codebook

The codebook chunk updates 2x2 and 4x4 vector tables.

The high byte of the chunk argument gives the number of 2x2 vectors. A value of
0 means 256. The low byte gives the number of 4x4 vector entries. A value of 0
can also mean 256, depending on the payload size.

Normal 2x2 vector layout:

| Field | Size | Description                 |
| ----- | ---- | --------------------------- |
| Y0    | 1    | Luma pixel 0                |
| Y1    | 1    | Luma pixel 1                |
| Y2    | 1    | Luma pixel 2                |
| Y3    | 1    | Luma pixel 3                |
| Cb    | 1    | Shared chroma blue-delta    |
| Cr    | 1    | Shared chroma red-delta     |

Alpha vector layout:

| Field | Size | Description                 |
| ----- | ---- | --------------------------- |
| Y0,A0 | 2    | Luma and alpha pixel 0      |
| Y1,A1 | 2    | Luma and alpha pixel 1      |
| Y2,A2 | 2    | Luma and alpha pixel 2      |
| Y3,A3 | 2    | Luma and alpha pixel 3      |
| Cb    | 1    | Shared chroma blue-delta    |
| Cr    | 1    | Shared chroma red-delta     |

That is the important 6-byte normal vector versus 10-byte alpha vector split.

### 0x1011 Quad Vector Frame

Vector frames consume motion/coding bits and update the current surface in
16x16 macroblocks. Each 16x16 macroblock contains four 8x8 blocks. Each 8x8
block may:

| Coding | Meaning                                      |
| ------ | -------------------------------------------- |
| 0      | Leave block unchanged                        |
| 1      | Copy from previous frame using motion vector |
| 2      | Paint from 8x8 / 4x4 codebook composition    |
| 3      | Recurse into 4x4 sub-blocks                  |

The chunk argument stores motion offsets:

```text
motionOffX = int8(argument >> 8)
motionOffY = int8(argument & 0xff)
```

Clandestiny has a known alternate motion interpretation for named streams. See
[Named Motion Variant Streams](#named-motion-variant-streams).

### 0x1012 JPEG Still

JPEG chunks contain a complete JPEG image. On Windows the tool decodes these
through WIC. On Linux it decodes them through libjpeg.

The decoded JPEG becomes the current frame and is written or displayed as
RGBA8888.

### 0x1013 Hang

Hang chunks hold or duplicate the current frame. If the chunk has a non-zero
size or argument, the decoder reports a malformed hang warning in logging mode.

### 0x1020 / 0x1021 Audio

ROQ sound chunks are delta-coded audio. The current tool expands them into
signed 16-bit little-endian stereo PCM at 22050 Hz.

Mono chunks duplicate the decoded sample into left and right channels. Stereo
chunks alternate left and right deltas.

When present, decoded audio is written as:

```text
audio.wav
```

and is included in GUI playback and MP4 export.

### 0x1030 Packet

`0x1030` is treated as a packet/container chunk by the current decoder. The
payload is skipped. It is listed explicitly because it appears in the Groovie-2
ROQ-family chunk set and should not be misidentified as corrupt data.

## Music and External Audio

Clandestiny also ships external MPEG audio/video assets in `MEDIA`, including
names like:

```text
ACT01MUS.MPG
ACT23MUS.MPG
MBF_ARM1.MPG
MBF_CBK1.MPG
MBF_UPH1.MPG
```

These are separate from ROQ/RNR embedded sound chunks. The current GUI playback
syncs decoded ROQ/RNR audio chunks. It does not yet emulate the full
Clandestiny runtime music system.

## System Files

Important observed files in `SYSTEM` include:

| File          | Observed size | Notes                                  |
| ------------- | ------------- | -------------------------------------- |
| `V32WIN.EXE`  | 355,328 bytes | Original Windows Groovie-2 host         |
| `SCRATCH1.ROL`| 66,216 bytes  | Standalone ROQ-family stream candidate |
| `LOAD.RAW`    | 141,282 bytes | System raw asset                       |
| `COVER.RAW`   | 921,936 bytes | System raw asset                       |
| `ICONS.PH`    | 423,545 bytes | Icon/cursor-ish system data            |

# Known Clandestiny Fuckery

## V32WIN.EXE Is Not The Whole Engine Contract

`V32WIN.EXE` is strong evidence that Clandestiny is in the same broad
Groovie-2 runtime family as other later Trilobyte titles, but it is not enough
to treat every stream as interchangeable.

The safer profile boundary is:

```cpp
enum class Groovie2Game {
    Unknown,
    CDY
};

struct Groovie2Profile {
    Groovie2Game game = Groovie2Game::CDY;
    bool usesResManV2 = true;
    bool usesDirRl = true;
    bool usesGjdGjd = true;
    bool usesRoqPlayer = true;
    bool allowAlphaCodebooks = true;
    bool allowJpegStillChunks = true;
    bool allowHangChunks = true;
    bool detectInterlacedShortVideo = true;
    bool cdyActDoorMotionVariant = true;
    bool cdyBricksOverlayHack = true;
};
```

In other words: this is a Clandestiny profile, not a generic "V32WIN means all
behavior is identical" assumption.

## Named Motion Variant Streams

Clandestiny has a stream-name-based motion variant for:

```text
act*
door*
trailer.rnr
```

The current decoder chooses the alternate motion path automatically when the
resource leaf name begins with `act`, begins with `door`, or equals
`trailer.rnr`.

Equivalent logic:

```cpp
if (name.starts_with("act") ||
    name.starts_with("door") ||
    name == "trailer.rnr") {
    useAlternateMotionDecoder = true;
}
```

This is not theoretical cleanup. It is required for correct Clandestiny motion
decode behavior.

## 26A_GRAF Overlay Behavior

`26A_GRAF.GRV` is associated with the Clandestiny bricks puzzle and appears to
depend on special surface copy/composite behavior in the original runtime.

That means a complete engine needs more than:

```text
decode video -> present frame
```

It needs explicit surface state:

```text
background
foreground
overlay
current buffer
previous buffer
```

The current tool decodes and displays ROQ/RNR frame output. It does not yet
emulate this full script-driven overlay behavior.

## Alpha Codebooks

Alpha is stream-level. Do not hardcode one codebook vector size.

Normal stream:

```text
Y,Y,Y,Y,Cb,Cr
```

Alpha stream:

```text
Y,A,Y,A,Y,A,Y,A,Cb,Cr
```

The decoder reads the `0x1001` argument as the alpha flag and then changes
codebook parsing accordingly.

## Short-Height / Interlaced Streams

Some streams are detected as short-height/interlaced-style sources when:

```text
height <= width / 3
```

Motion-copy math is adjusted for those streams. This is another reason the
viewer needs live frame inspection rather than just blind batch extraction.

# Viewer Architecture

## Asset Table

The GUI uses a Win32 Common Controls ListView in report mode. Columns are:

| Column | Meaning                         |
| ------ | ------------------------------- |
| Name   | Resource name from `DIR.RL`      |
| Size   | Byte length from `DIR.RL`        |
| GJD    | Pack index into `GJD.GJD`        |
| Disks  | Disc bitmask                     |
| Offset | Byte offset inside the GJD pack  |

Default sort:

```text
Size descending
```

Clicking any column header resorts the table. Clicking the same column again
toggles ascending/descending order.

## Async Decode

Selection decode is asynchronous. The UI thread does not directly read and
decode the selected resource. Instead, it:

1. Captures the selected `ResourceEntry`.
2. Starts a worker thread.
3. Reopens the catalog in that worker.
4. Tries the configured disc roots according to the resource disc bitmask.
5. Reads the selected GJD byte range from the disc that actually has it.
6. Decodes frames and audio in memory.
7. Posts the completed `DecodedMovie` back to the UI thread.

Each decode request has a serial number. If the user selects another asset
before an earlier decode finishes, the stale result is ignored.

## Playback

The `Play` button performs in-place playback inside the preview pane. It does
not create a separate media player window and does not shell out to ffmpeg.

Playback timing uses:

```text
GetTickCount64() elapsed time
decoded stream fps
Win32 timer ticks for UI invalidation
```

Audio is started at playback begin using the decoded in-memory WAV data. Video
frames are selected from elapsed time instead of "sleep one frame, advance one
frame", which keeps the preview tied to the same wall-clock timeline as audio.

The preview window suppresses background erase and paints through a back buffer
before blitting to the screen. This keeps GDI playback from flickering while
avoiding a larger Direct3D dependency for the current Win32-only GUI.

## Rendering Modes

The preview has two scaling modes:

| Mode     | Behavior                                                |
| -------- | ------------------------------------------------------- |
| Fit      | Scale the frame to fill the usable preview area while preserving aspect ratio |
| Original | Draw at original decoded pixel size, centered in the preview area |

`Fit` is the default mode.

## MP4 Encoding

`Encode MP4` runs asynchronously. The UI thread remains responsive while the
worker writes temporary raw RGBA/WAV inputs and runs ffmpeg.

The ffmpeg command uses:

```text
libx264rgb
crf 0
preset veryslow
pix_fmt rgb24
ALAC audio when ROQ/RNR audio exists
```

During encode, the GUI opens a modeless log window that captures ffmpeg stdout
and stderr. The log window uses standard Windows controls and includes a
`Copy Log` button.

# Developers

## Build System Overview

The repository has a compact build script:

```text
build.sh
build.cmd
build.command
```

It supports:

| Command              | Output                         |
| -------------------- | ------------------------------ |
| `./build.sh release` | `groovie2.exe`                 |
| `./build.sh debug`   | `groovie2-debug.exe`           |
| `./build.sh linux`   | `build/groovie2-linux`         |
| `./build.sh macos`   | `build/groovie2-macos`         |
| `build.cmd`          | `groovie2.exe`                 |
| `./build.command`    | `build/groovie2-macos`         |
| `./build.sh clean`   | Removes build outputs          |

The Windows GUI build uses `clang-cl`, `lld-link`, and a Windows SDK located
under `/opt/winsdk` by default when cross-building from Linux, or MSVC when
using `build.cmd` on Windows. The Linux and macOS builds compile the same
decoder as CLI-only executables and link libjpeg.

## Prerequisites

Windows cross-build prerequisites:

```text
clang-cl
lld-link
Windows SDK / UCRT under /opt/winsdk
```

Windows linked libraries:

```text
windowscodecs.lib
ole32.lib
uuid.lib
user32.lib
gdi32.lib
shell32.lib
comctl32.lib
winmm.lib
```

Linux CLI build prerequisites:

```text
clang++
libjpeg
```

Runtime MP4 export prerequisite:

```text
ffmpeg on PATH
```

macOS CLI build prerequisites:

```text
Apple Command Line Tools
libjpeg or jpeg-turbo
```

With Homebrew:

```sh
brew install jpeg
```

## Build Invocation

Windows, from a normal, non-Administrator Developer PowerShell for Visual
Studio 2022:

```cmd
.\build.cmd
.\build.cmd clean
.\build.cmd rebuild
```

`.\build.cmd` builds `groovie2.exe` as the Windows GUI executable. When command
line arguments are supplied to `groovie2.exe`, the same binary runs the CLI
extractor instead of opening the GUI.

Windows cross-build from Linux:

```sh
./build.sh release
./build.sh debug
./build.sh clean
```

`./build.sh release` builds `groovie2.exe` with `clang-cl` and `lld-link`
against the Windows SDK under `/opt/winsdk`. Set `WINSDK_BASE` to use another
SDK location.

Linux:

```sh
./build.sh linux
./build.sh clean
```

`./build.sh linux` builds `build/groovie2-linux`, a CLI-only executable. Set
`CXX`, `CXXFLAGS`, or `LDFLAGS` to override the default compiler or flags.

macOS:

```sh
./build.command
./build.command clean
./build.command rebuild
```

`./build.command` builds `build/groovie2-macos`, a CLI-only executable. It asks
Apple's toolchain for the default C++ compiler with `xcrun --find c++`, then
falls back to `c++` if needed. If the compiler is missing, install Apple's
Command Line Tools with:

```sh
xcode-select --install
```

Run quick catalog smoke tests:

```sh
./build/groovie2-linux -list Clandestiny/disc1
./build/groovie2-macos -list Clandestiny/disc1
```

## Technical Anchors

Useful upstream references for validating Groovie-family behavior:

```text
https://github.com/scummvm/scummvm/tree/master/engines/groovie
https://github.com/scummvm/scummvm/blob/master/engines/groovie/detection.cpp
https://github.com/scummvm/scummvm/blob/master/engines/groovie/detection.h
https://github.com/scummvm/scummvm/blob/master/engines/groovie/resource.cpp
https://github.com/scummvm/scummvm/blob/master/engines/groovie/groovie.cpp
https://github.com/scummvm/scummvm/blob/master/engines/groovie/video/roq.cpp
https://github.com/scummvm/scummvm/blob/master/engines/groovie/script.cpp
```

# CHANGELOG

## 2026-05-22

- Changed the asset table default sort from `Size` ascending to `Size`
  descending.
- Expanded GUI preferences and `config.json` handling to track Disc 1 and Disc
  2 roots separately.
- Added legacy `clandestiny_root` migration behavior that infers a sibling disc
  folder when possible.
- Made GUI resource loading try configured disc roots according to the resource
  disc bitmask, so Disc 1-only and Disc 2-only assets can be selected from the
  same catalog view.
- Changed preview painting to suppress background erase and draw through a GDI
  back buffer before blitting to the screen.
- Added a small current-frame BGRA cache for preview painting.
- Reduced playback label updates during playback to avoid extra UI repaint
  churn.
- Added native Windows `build.cmd` support for Visual Studio Developer
  PowerShell / Developer Command Prompt builds.
- Added macOS `build.command` support for CLI-only builds.
- Added `./build.sh macos` as a direct macOS CLI build target.
- Kept Linux and macOS builds independent of the Win32 GUI path.
- Documented the three user-facing build flows: Windows, Linux, and macOS.
- Documented that Windows uses one GUI-capable executable that switches to CLI
  mode when argv arguments are supplied.
- Documented that Linux and macOS produce CLI-only executables for resource
  listing, extraction, decoding, and MP4 export workflows.
- Added Homebrew-aware macOS libjpeg discovery in the build script.
- Added this README changelog.

## 2026-05-21

- Created the Clandestiny / CDY root README as the master technical write-up.
- Scoped the documentation to Clandestiny instead of merging 11th Hour and CDY
  into one repo narrative.
- Documented `GJD.GJD`, `DIR.RL`, GJD packs, GRV scripts, ROQ/RNR streams, sound
  chunks, JPEG chunks, alpha codebooks, and Clandestiny-specific motion
  variants.
- Added a Win32 Common Controls asset table with sortable columns.
- Changed the asset list default ordering to `Size` ascending.
- Replaced the previous frame stepping controls with a `Play` button.
- Added timed in-place ROQ/RNR playback in the preview pane.
- Started decoded audio with video playback so audio and frame selection share
  the same wall-clock timeline.
- Added `Fit` / `Original` preview scaling.
- Made decode-on-selection asynchronous.
- Made MP4 encoding asynchronous.
- Added a modeless ffmpeg log window with captured text output and a `Copy Log`
  button.
- Kept the command-line extractor path available on Windows when arguments are
  supplied.
