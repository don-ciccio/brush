# brush examples

Sample projects you can open in the editor or run in the game.

Open one by pointing brush at its folder:

```sh
make run-editor            # then File → Open, pick examples/meadow
# or, headless / CLI:
BRUSH_PROJECT=examples/meadow ./build/editor
BRUSH_PROJECT=examples/meadow ./build/sandbox   # play it
```

## meadow

Instanced foliage with **real grass models**: one layer mixes three Quaternius
grass meshes as a palette (each at its own scale — `Grass1` 5×, `Grass4` 2.5×,
`Grass_5` 6.2×), plus a sparse poppy layer. Shows off the multi-model palette,
per-variant scale, and the tint control. Assets are CC0 — see
`meadow/CREDITS.md`. Edit `meadow/assets/meadow.def` or tune it live in the
editor's Foliage panel.
