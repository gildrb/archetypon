# archetypon

SVG parsing and rasterization, PNG encoding, lossless WebP encoding, ICO assembly, and conservative SVG optimization without third-party runtime libraries or subprocesses.

![Generated SVG, PNG, WebP, and favicon asset directories](assets/archetypon-output.png)

### Install

```sh
git clone https://github.com/gildrb/archetypon
cd archetypon
make
sudo make install
```

### Use

```sh
cd /path/to/file
archetypon create file.svg
```

`create` reads one SVG and writes `svg/`, `png/`, `webp/`, and `favicon/` in the current directory. Numeric filenames identify the longest edge. The other edge is derived from the SVG `viewBox`; aspect ratio is preserved and square padding is never added.

### Commands

| Command | Purpose |
| --- | --- |
| `make` | Build the `archetypon` CLI and `libarchetypon.a` with the system C toolchain and `libm` |
| `sudo make install` | Install the CLI, public header, and static library under `${PREFIX:-/usr/local}` |
| `archetypon create <file.svg>` | Validate the input and generate the complete asset tree in the current directory |
| `archetypon --help` | Print the accepted command syntax |
| `make test` | Run public-API, output, preservation, rejection, pixel, and decoder checks |
| `make clean` | Remove generated objects, the CLI, and the static library |

Build variables are conventional Make inputs:

```sh
make CC=clang CFLAGS='-std=c11 -O2 -Wall -Wextra -Wpedantic'
make install PREFIX="$HOME/.local"
```

### Files

```text
archetypon/
  archetypon.h       public image, SVG, PNG, WebP, and ICO package API
  main.c             CLI policy, input limits, filesystem reads and writes
  src/
    internal.h       private scalar aliases and cross-module helpers
    core.c           checked allocation, growable buffers, diagnostics
    svg.c            XML/SVG parsing, paths, styles, rasterization
    image.c          RGBA image lifetime and proportional resizing
    png.c            PNG encoder
    webp.c           lossless VP8L WebP encoder
    ico.c            PNG-backed ICO assembly
    optimize.c       conservative SVG serialization
  tests/
    api.c             public package and ownership checks
    test.sh           end-to-end CLI, output, pixel, and decoder checks
  assets/             generated output screenshot
  Makefile            CLI, static-library, install, test, clean targets
  README.md           contracts and verification
  .gitignore          generated build and asset paths
```

`main.c` depends only on `archetypon.h`. Reusable implementation lives under `src/`; internal helpers are not part of the public contract. The CLI and static library link only the C runtime and `libm`.

### C API

```c
#include <archetypon.h>

ArchetyponImage image = {0};
ArchetyponBuffer png = {0};
char error[256] = {0};

if (!archetypon_svg_render(svg, svg_length, width, height, &image,
                            error, sizeof(error)) ||
    !archetypon_png_encode(&image, &png, error, sizeof(error))) {
  /* error contains the diagnostic. */
}

archetypon_buffer_free(&png);
archetypon_image_free(&image);
```

`archetypon.h` exposes canvas-size inspection, exact-canvas SVG rendering, proportional RGBA resizing, SVG optimization, PNG and lossless WebP encoding, and three-entry PNG-backed ICO assembly. Callers zero-initialize output buffers and images, retain ownership of input bytes, and release successful or partial outputs with the matching free function. Encoded buffers and rendered images are heap-owned; SVG source does not need a trailing NUL byte.

### Output

```text
<current directory>/
  <input.svg>                  caller-owned source; never modified
  svg/
    original.svg              byte-for-byte input copy
    optimized.svg             comments and inter-element whitespace removed
  png/
    16.png
    32.png
    48.png
    64.png
    128.png
    256.png
    512.png
    1024.png
    2048.png
  webp/
    256.webp
    512.webp
    1024.webp
  favicon/
    favicon.svg               same bytes as svg/optimized.svg
    favicon.ico               embedded 16, 32, and 48 PNG payloads
    favicon-16.png            same bytes as png/16.png
    favicon-32.png            same bytes as png/32.png
```

For a `240 × 120` viewBox, `png/2048.png` is `2048 × 1024`, `png/16.png` is `16 × 8`, and the ICO entries are `16 × 8`, `32 × 16`, and `48 × 24`. Names describe the requested longest edge, not a square canvas.

Existing files at owned output paths are replaced. Unrelated files in those directories are not removed. Input parsing and the 2048-edge master render complete before output directories are created; a later filesystem failure can leave a partially updated output tree.

### Dataflow

```text
<input.svg>
     |
     +--> read: maximum 32 MiB
     |
     +--> root geometry: viewBox, otherwise positive width + height
     |
     +--> XML/tag parser
     |      -> inherited presentation state
     |      -> affine transforms
     |      -> paths and primitive shapes
     |
     +--> path flattening
     |      -> cubic and quadratic subdivision
     |      -> elliptical-arc sampling
     |
     +--> 2x supersampled RGBA raster
     |      -> proportional 2048-edge master
     |
     +--> bilinear premultiplied-alpha resize
     |      +--> PNG: 16..2048
     |      +--> lossless VP8L WebP: 256, 512, 1024
     |      +--> PNG-backed ICO: 16, 32, 48
     |
     +--> conservative XML copy optimization
            +--> svg/optimized.svg
            +--> favicon/favicon.svg
```

Raster generation scales and centers the declared viewBox into an output whose dimensions already match that aspect ratio. It does not stretch, crop, or add a square background.

### SVG contract

Supported geometry:

| Surface | Accepted input |
| --- | --- |
| Root | one non-nested `svg`; positive `viewBox` or unitless/`px` width and height |
| Containers | `g`, `a`, `defs`, `metadata`, `title`, `desc`; editor-namespaced metadata is ignored |
| Shapes | `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon` |
| Path data | absolute and relative `M L H V C S Q T A Z`, repeated parameters, multiple contours |
| Transforms | `matrix`, `translate`, `scale`, `rotate`, `skewX`, `skewY` |
| Paint | solid fill/stroke, `none`, `transparent`, `currentColor`, hexadecimal colors, numeric `rgb()`/`rgba()`, basic SVG color names |
| Compositing | element, fill, and stroke opacity; `nonzero` and `evenodd` fill rules |
| Stroke | width plus butt, round, and square cap syntax; solid strokes only |
| Rectangle | optional `x`, `y`, `rx`, `ry`; radii clamped to half-size |

Intentional boundaries:

| Rejected surface | Reason |
| --- | --- |
| gradients and paint servers | no gradient sampler or referenced paint graph |
| text and `tspan` | no font parser, shaping, or glyph rasterizer |
| `image`, `use`, `foreignObject` | no external resource or document resolver |
| CSS stylesheets | no selector or cascade engine; presentation attributes and inline declarations only |
| filters, masks, clipping paths, patterns | no offscreen effect graph |
| dashed strokes | no dash subdivision |
| nested SVG viewports | one root coordinate system only |
| percentage and non-pixel dimensions | no CSS unit-resolution context |

Unsupported rendering elements and recognized unsupported paint/effect properties terminate the command with a nonzero exit status. Editor metadata and unknown CSS declarations that do not define geometry are ignored.

Opacity on a container is multiplied into descendants; containers are not composited through separate offscreen layers. Stroke geometry uses the renderer's minimal distance-based rasterizer. These are implementation boundaries, not claims of full SVG conformance.

### Encoders

| Format | Contract |
| --- | --- |
| PNG | RGBA, 8 bits per channel, non-interlaced, filter type 0, zlib stored DEFLATE blocks, CRC-32 and Adler-32 generated locally |
| WebP | RIFF `WEBP` container, lossless `VP8L`, literal ARGB channels, canonical fixed prefix codes, no color cache, transforms, or backward references |
| ICO | icon directory with three PNG-backed entries; directory dimensions match each proportional PNG payload |
| optimized SVG | source bytes preserved except XML comments, whitespace between elements, trailing whitespace, and one normalized final newline |

The encoders prioritize a small auditable implementation over compression ratio. PNG and WebP outputs are valid but intentionally do not implement adaptive compression.

### Exit contract

| Status | Meaning |
| --- | --- |
| `0` | help printed or all requested assets written |
| `1` | invalid/unsupported SVG, allocation failure, or filesystem/encoding failure |
| `2` | unsupported command or argument count |

Diagnostics are written as `archetypon: <cause>` to standard error. Usage errors print accepted syntax to standard error. Successful generation prints one line to standard output.

### Verification

```sh
cd "$HOME/Repos/archetypon"
make clean
make test

```

`make test` first compiles `tests/api.c` against `libarchetypon.a`, then runs the CLI suite. ImageMagick's `identify` and `compare` commands are required for independent decoder checks. The suite verifies:

1. public API rendering, resizing, optimization, encoding, ownership, and rejection paths;
2. CLI statuses, output tree, source preservation, and SVG optimization;
3. representative rendered pixels across supported shapes, styles, transforms, and opacity;
4. dimensions and decoder validity for PNG, lossless WebP, and PNG-backed ICO;
5. decoded pixel equality across corresponding PNG, WebP, and ICO outputs.
### Safety

`create` owns and overwrites the paths listed under **Output**. Run it from the intended asset directory. It does not delete unrelated files, modify the input SVG, access the network, execute subprocesses, load external SVG resources, or write outside the current directory's fixed output subdirectories.
