# kiview

Image and document viewer for **kidesktop-base**: a GTK2 frontend with a footer of
buttons, and viewers as separate plugin processes that draw into a window kiview gives
them. See [PLAN.md](PLAN.md) for the design and [PROTOCOL.md](PROTOCOL.md) for the plugin
protocol.

0.0.1 opens images (JPEG, PNG, WebP, GIF, BMP, TIFF, ... through gdk-pixbuf, WebP through
libwebp when present), steps through the folder, zooms (Ctrl+wheel, +/-, F, 1), pans by
dragging, rotates the view (Ctrl+, / Ctrl+.) and goes full screen (F11).

```
make            # kiview, kiview-image, kiview-test
make check      # unit tests of the view math and the wire format
make run FILE=photo.jpg
make run FILE=grid.kvtest    # the test plugin's grid (any file named *.kvtest)
```

`KIVIEW_TRACE=1` prints every message between kiview and its plugin.
