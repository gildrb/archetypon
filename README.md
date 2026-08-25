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
