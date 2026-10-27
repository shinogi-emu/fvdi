# fvdi
The fVDI graphics driver used by [Shinogi](https://github.com/shinogi-emu/shinogi),
forked from [freemint/fVDI](https://github.com/freemint/fvdi).

## Download

* [fVDI for 68020-68060, with FreeType 2.8.1](https://github.com/shinogi-emu/fvdi/releases/latest/download/fvdi-020-60-ft2.8.1.zip)
  -- the build Shinogi ships on its drive C
* [All releases](https://github.com/shinogi-emu/fvdi/releases)

These are this fork's builds. Upstream's own builds are not the same
program: this fork carries a reworked 16bpp driver, FreeType font fixes and
NVDI-style raster conversion that upstream does not have.

## How to build
```
cd fvdi
make CPU=<CPU type> V=1
make CPU=<CPU type> DESTDIR=<install dir> install
```

Where `<CPU type>` is one of `v4e` (for ColdFire) or any other m68k-atari-mint
CPU target (`000` for 68000, `020` for 68020 etc).

To get truetype support: download a [freetype source archive](https://download.savannah.gnu.org/releases/freetype/), untar it somewhere, and create a symlink fvdi/modules/ft2/freetype to the top level directory.

The release above is built with `make CPU=020-60` (one binary for the 68020
to 68060) and FreeType 2.8.1, using m68k-atari-mint-gcc 4.6.4.
