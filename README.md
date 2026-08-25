# archetypon

Generate optimized SVG, PNG, lossless WebP, and favicon assets from SVG.

![Generated assets](assets/archetypon-output.png)

### Install

```sh
git clone https://github.com/gildrb/archetypon
cd archetypon
make
sudo make install
```

### Use

```sh
archetypon create file.svg
archetypon create png file.svg
```

Omit the format to create all assets. Available formats are `svg`, `png`,
`webp`, and `ico`.

### Test

`make test` requires ImageMagick.

```sh
make test
```

### Retained SVG rendering

`archetypon_svg_document_create` validates and owns an immutable copy of the SVG
bytes and parses the root canvas geometry once. `archetypon_svg_plan_create`
builds an immutable, reference-counted RGBA plan for one output size. The legacy
`archetypon_svg_render` API is compatibility glue over those two objects.

Document creation builds a compact scene of supported shape records. XML element
traversal, hierarchy checks, inherited style resolution, visibility, and transform
syntax parsing happen only there. Source and compiled-scene storage are each
limited to 32 MiB. The source byte and length accessors remain valid until the
immutable document is freed and allow exact serialization without another copy.

Each size plan still parses numeric shape/path attributes, applies the stored
transforms with the output viewport, flattens curves, rasterizes, supersamples,
and stores the completed straight-alpha RGBA result. Plans are immutable and may
be retained and read concurrently. Their pixels remain valid through the caller's
plan reference. Plan reuse repeats none of the size-dependent work. Moving numeric
geometry into the scene is a possible future split and does not require an API
change.

### Supported SVG subset

The retained renderer supports paths and basic shapes, affine transforms, solid
and linear-gradient fills, presentation attributes, inline styles, simple
embedded element/class/ID CSS selectors, dashed strokes with round/miter/bevel
joins, group and element opacity, clipping paths, and luminance or alpha masks.
Group effects are isolated and composited once. Gradient, CSS, scene, surface,
path, effect, and render-work limits bound untrusted input and temporary memory.

Unsupported constructs fail during document creation instead of rendering a
partial result. Current explicit limits include text, images, external
resources, radial gradients, filters, patterns, nested viewports, complex CSS
selectors, nested clip/mask content, and non-pad gradient spread modes.
