# TreeFrogUI Ebook Reader

A tiny standalone ebook/document reader for the MIPS Hichip handhelds
(R36SX / SF3000 / SF3000 HD / SF3100 / SF3500 / GB350) that TreeFrogUI runs on.
Reads **EPUB, MOBI, PDF, CBZ, FB2, XPS** - everything MuPDF handles.

It is launched by TreeFrogUI as a standalone (like `pcsx4all` / `lgpt` /
`rockbox`): the `Ebook` rom folder maps to the `ebook` binary, which is run with
the book path as its argument.

## What it is (and isn't)

- **`reader.c`** - first-party glue (~350 lines): MuPDF renders a page ->
  converted to RGB565 -> presented via `driver.so` (the shared `hwdisp` path
  used by the other standalones) -> input from cubevol's `/tmp/joy_key` shm.
- **MuPDF** - the rendering engine, used **unmodified** as an upstream library
  (vendored + cross-compiled, no source patches). Not forked.
- **`port_sf3000/`** - the display harness (`hwdisp.c`, `tf_driver.h`) copied
  from the pcsx4all port so all standalones present the same way on every device.

## Controls

| Button | Reading |
|--------|---------|
| R1 / RIGHT / A | next page |
| L1 / LEFT / B | previous page |
| R2 / L2 | jump +/- 10% |
| SELECT | open/close menu |
| SELECT + START | quit (progress saved) |

Menu: UP/DOWN move; on **Text size** / **Font** rows LEFT/RIGHT change the value
(applied when you close the menu); **A** runs action rows. Reading position,
font size and font face are saved per book under a hidden `.positions/`
subdirectory beside the books. Older `<book>.pos` sidecars are migrated when
the book is opened.

### Fonts

Built-in Serif / Sans / Mono, plus any `.ttf` / `.otf` dropped in
`frogui/fonts/`, `cubegm/fonts/`, or `roms/Ebook/fonts/` (loaded via `@font-face`).

## Build

Needs the SF3000 MIPS toolchain (see the main TreeFrogUI workspace).

```sh
# 1. Vendor + cross-build MuPDF (unmodified upstream, pinned tag).
git clone --depth 1 --branch 1.24.10 --recurse-submodules --shallow-submodules \
    https://github.com/ArtifexSoftware/mupdf.git ../mupdf
./build_mupdf_sf.sh        # -> ../mupdf/build/release/libmupdf*.a

# 2. Build the reader (links libmupdf statically; libstdc++ static-linked too).
./build_reader_sf.sh       # -> ./ebook  (MIPS binary, ~38 MB with fonts)
```

Copy `ebook` to `cubegm/ebook` on the SD card.

## License

The reader glue is provided under the **GNU AGPL-3.0** to match MuPDF, which it
links statically. **MuPDF is AGPL-3.0** ([Artifex](https://mupdf.com/licensing));
distributing this binary makes the whole thing AGPL. If you need a
non-AGPL/commercial distribution, obtain a commercial MuPDF license from Artifex.
