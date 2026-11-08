# The interface kit

ProsperoAI's interface is built with
[ps5-homebrew-ui](https://github.com/JVK3R/ps5-homebrew-ui), a mirror of the pinned
upstream commit. The kit is a
build dependency: none of its source, fonts or sounds are kept in this repository, and
nothing is patched over it.

`sources.txt` lists the kit sources the app compiles, relative to the kit's `src/`.

`tools/prepare-ui-kit.sh` fetches the pinned commit into `.deps/ui-kit/checkout`, copies
its `src/` to `.deps/ui-kit/stage/src` and prints the staged folder. The console build,
the host preview and the host tests all compile from there. `tools/bake-fonts.sh` bakes
the fonts with the kit's baker, and `tools/build.sh` packages the kit's sounds, straight
from the checkout.

## Moving the pin

Change `commit=` in `tools/prepare-ui-kit.sh`.

When the app needs something the kit does not do, the change is made in the kit and the
pin is moved; no patched copy is kept here. To work on both at once, point the build at a
checkout:

```sh
UI_KIT_DIR=~/ps5-homebrew-ui make app
```

The pinned commit is the one that gave `gfx::Font` its fallback faces
(`Font::add_fallback`) and made `Font::wrap` break a word wider than its line, both of
which `src/font_set.cpp` and the conversation view rely on.
