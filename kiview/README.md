# kiview

Image and document viewer for **kidesktop-base**: a GTK2 frontend with a footer of
buttons, and viewers as separate plugin processes that draw into a window kiview gives
them. See [PLAN.md](PLAN.md) for the design and [PROTOCOL.md](PROTOCOL.md) for the plugin
protocol.

Opens images: JPEG (decoded at 1/2, 1/4 or 1/8 when shown smaller, EXIF orientation), PNG,
GIF and WebP (animated too) with their own decoders; BMP, TIFF, ICO, TGA, ... through
gdk-pixbuf. Steps through the folder (the neighbours are decoded ahead), zooms (Ctrl+wheel,
+/-, F, 1), pans by dragging, rotates the view (Ctrl+, / Ctrl+.), goes full screen (F11)
and runs a slideshow (F5; Space pauses, Esc stops).

```
make            # kiview, kiview-image, kiview-test
make check      # unit tests of the view math and the wire format
make run FILE=photo.jpg
make run FILE=grid.kvtest    # the test plugin's grid (any file named *.kvtest)
```

`KIVIEW_TRACE=1` prints every message between kiview and its plugin.
