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
- Layers (panel on the right, hideable in *View*): new, duplicate, delete,
  raise/lower, rename (double click), show/hide, opacity. All of it is in the
  undo history. The tools paint on the active layer
- Two document modes. A **flat image** (opened from PNG/JPEG/BMP/TIFF, or
  new) is saved with `Ctrl+S` in its own format; with more than one layer, a
  notice says the layers will be merged (*Continue*, *Save as...* or
  *Cancel*). A **layered project** (opened from, or saved as, `.ora`) is saved
  with `Ctrl+S` to the `.ora`, and *File > Export...* writes a flat copy
  without touching the project
- Opens whatever gdk-pixbuf reads (EXIF rotation applied) and OpenRaster
  (`.ora`, readable by GIMP, Krita and MyPaint); saves `.ora`, PNG, JPEG, BMP
  and TIFF, chosen by the extension (JPEG and BMP are flattened over white)

Settings (palette, colors, tool sizes, grid, layers panel, size of new images) are in
`~/.config/kipaint.conf`. Command line: `kipaint [IMAGE]`.

Not yet: moving/rotating layers, selections, clipboard, shapes, text.
Nested groups in an `.ora` are read as a flat list of layers, and layer
offsets are applied on load (every layer is document-sized in kipaint). See [PLAN.md](PLAN.md) for the roadmap.
