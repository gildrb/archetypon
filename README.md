# archetypon

Turn an SVG into optimized SVG, PDF, PNG, WebP, JPEG, and ICO assets.

![Generated assets](assets/archetypon-output.png)

### Install

```sh
git clone https://github.com/gildrb/archetypon
cd archetypon
make
sudo make install
```

This installs `archetypon` and its short alias `typ`.

### Use

```sh
typ logo.svg
typ -f png,ico -s 64,512 logo.svg
typ -o assets *.svg
```

Each input gets a folder named after it. Formats are `svg`, `pdf`, `png`,
`webp`, `jpeg`, and `ico`; see `typ --help` for all options.

### Test

`make test` requires ImageMagick.
