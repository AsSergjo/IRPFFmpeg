# Third-Party Notices

IRPFFmpeg includes or depends on third-party components. The MIT license in `LICENSE` applies to the IRPFFmpeg source code in this repository, not to third-party libraries.

This file is a practical notice for source and binary distribution. It is not legal advice.

## Project Code

```text
Component: IRPFFmpeg source code
License: MIT License
Copyright: Copyright (c) 2026 AsSergjo
Notice: See LICENSE and AUTHORS.md.
```

## FFmpeg

```text
Component: FFmpeg libraries / DLL files
Typical files:
  avcodec-62.dll
  avfilter-11.dll
  avformat-62.dll
  avutil-60.dll
  swresample-6.dll
  swscale-9.dll
License: LGPL/GPL according to the exact FFmpeg build configuration
Website: https://ffmpeg.org/
Legal notes: https://ffmpeg.org/legal.html
```

The exact FFmpeg license depends on how the FFmpeg DLLs were built.

- If FFmpeg is built without `--enable-gpl` and without `--enable-nonfree`, the FFmpeg libraries are normally distributed under LGPL 2.1 or later.
- If GPL components are enabled, GPL terms apply to that FFmpeg build.
- If non-free components are enabled, redistribution may be restricted.

For binary releases, keep FFmpeg dynamically linked as DLL files and include the matching FFmpeg license notices, source offer/source archive, build configuration, and any changes made to FFmpeg.

## SDL2

```text
Component: SDL2
Typical file: SDL2.dll
License: zlib license
Website: https://www.libsdl.org/
```

## SDL2_image

```text
Component: SDL2_image
Typical file: SDL2_image.dll
License: zlib license
Website: https://github.com/libsdl-org/SDL_image
```

## Image and Compression Libraries

The runtime package may also include image/compression libraries used directly or through SDL2_image/FFmpeg.

```text
Typical files:
  libpng16.dll
  jpeg62.dll
  turbojpeg.dll
  zlib1.dll
Licenses: according to each library's own license
```

For binary releases, include the original license texts for every shipped DLL.

## Recommended Release Layout

```text
IRPFFmpeg-vX.Y.Z-win-x64/
  Start_IRPFFmpeg.exe
  IRPFFmpeg.exe
  LICENSE
  THIRD_PARTY_NOTICES.md
  playlist.m3u
  heap_dll/
    avcodec-62.dll
    avfilter-11.dll
    avformat-62.dll
    avutil-60.dll
    jpeg62.dll
    libpng16.dll
    SDL2.dll
    SDL2_image.dll
    swresample-6.dll
    swscale-9.dll
    turbojpeg.dll
    zlib1.dll
  licenses/
    FFmpeg-LICENSE.txt
    FFmpeg-build-configuration.txt
    SDL2-LICENSE.txt
    SDL2_image-LICENSE.txt
    libpng-LICENSE.txt
    zlib-LICENSE.txt
    libjpeg-turbo-LICENSE.txt
```
