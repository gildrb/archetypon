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
