# kipaint

Paint program for KiDesktop (GTK2), in the spirit of the classic Paint. Part
of **kidesktop-base**. Plain cairo + gdk-pixbuf, no threads: about 30 MB of
RAM with a small image, under 40 MB right after opening a 1920x1080 one.

- Tools: pencil (aliased, 1 px by default), brush (antialiased, round),
  eraser (paints color 2 like Paint's; *Transparent* erases to alpha), fill
  (the area of exactly the clicked color), color picker (goes back to the
  previous tool when released). Shortcuts: `P` `B` `E` `F` `I`; `[` and `]`
  change the size
- Colors: left button paints with color 1, right button with color 2. In the
  palette at the bottom, left/right click picks them, a double click edits
  that entry; clicking color 1 or 2 edits it; `X` swaps them
- Zoom: `Ctrl`+wheel around the pointer (12.5% to 3200%), `Ctrl+Plus` /
  `Ctrl+Minus`, `Ctrl+0` actual size, `Ctrl+Shift+0` fit to window. The wheel
  scrolls (`Shift` for sideways), the middle button drags the view.
  *View > Pixel grid* (`Ctrl+G`) shows from 800% up
- Undo/redo (`Ctrl+Z`, `Ctrl+Y` or `Ctrl+Shift+Z`): only the 64x64 tiles a
  stroke touched are kept, up to 64 MB of history
- Opens whatever gdk-pixbuf reads (EXIF rotation applied); saves PNG, JPEG,
  BMP and TIFF, chosen by the extension (JPEG and BMP are flattened over
  white)

Settings (palette, colors, tool sizes, grid, size of new images) are in
`~/.config/kipaint.conf`. Command line: `kipaint [IMAGE]`.

Not in 0.0.1: layers, the OpenRaster (.ora) format, moving/rotating layers,
selections, clipboard, shapes, text. See [PLAN.md](PLAN.md) for the roadmap.
