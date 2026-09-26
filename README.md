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

### Use

```sh
archetypon logo.svg
archetypon -f png,ico -s 64,512 logo.svg
archetypon -o assets *.svg
```

Each input gets a folder named after it. Formats are `svg`, `pdf`, `png`,
`webp`, `jpeg`, and `ico`; see `archetypon --help` for all options.

### Web

Drag and drop in the browser, with nothing uploaded. Requires Nix.

```sh
make web
python3 -m http.server -d web
```

### Test

`make test` requires ImageMagick.
