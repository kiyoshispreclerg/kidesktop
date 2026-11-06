# kiview plugin protocol, version 1

A plugin is an executable that kiview starts once per kind of document and keeps
alive. It draws into an X window kiview made, and kiview tells it what to do. See
PLAN.md for why.

## Transport

- kiview runs the plugin with one end of a `socketpair(AF_UNIX, SOCK_SEQPACKET)` as
  **fd 3**. The plugin exits when the peer closes it (kiview also sets
  `PR_SET_PDEATHSIG`).
- One message per packet, at most 8192 bytes: `verb key=value key=value`. Values are
  escaped as `%XX` for every byte but `A-Za-z0-9._~/:+,-`. Numbers use the C locale.
- A message may carry one file descriptor (`SCM_RIGHTS`), as `open` does.
- **Unknown verbs and keys are ignored**, never an error.
- `KIVIEW_TRACE=1` in kiview's environment makes it print every message.

## Manifest

`$XDG_DATA_DIRS/kiview/plugins/<name>.kvplugin` (and `~/.local/share/kiview/plugins`,
and the colon-separated `KIVIEW_PLUGIN_DIR`), a key file:

```ini
[Plugin]
Name=image
Exec=kiview-image      # relative: $LIBEXECDIR/kiview (or KIVIEW_LIBEXEC_DIR)
Protocol=1
MimeTypes=image/jpeg;image/png;
Extensions=jpg;jpeg;png;     # when the MIME type doesn't say
Priority=50                  # the highest wins a type two plugins share
```

## kiview → plugin

| Message | Keys | Meaning |
|---|---|---|
| `hello` | `version` | First message. |
| `surface` | `xid w h density seq` | Draw into window `xid`. `w`×`h` is the area in logical pixels; the window is already `w×density`, `h×density` physical pixels. `density` is `num/den` (`1/1` when not dense). Answer with `frame seq=` once drawn. |
| `style` | `bg checker` | Background `#rrggbb`; `checker=1` shows transparency as a chessboard. |
| `open` | `id name mime` + fd | Open the document the fd reads and show it. `name` is for the title and as a format hint. Replaces the current one (which the plugin keeps until `close`). |
| `prefetch` | `id name mime` + fd | Decode in the background without showing (the neighbours in the folder). |
| `show` | `id` | Show a document opened or prefetched before; answered with `opened` or `failed` like `open`. |
| `close` | `id` | Release it. kiview keeps the plugin holding only the shown document and its two neighbours. |
| `anim` | `mode` (`play` `pause`) | Pause or resume the shown animation. |
| `zoom` | `mode` (`fit` `fit-width` `actual` `set` `by`), `value` or `factor`, `x y` | Zoom; `x y` is the anchor in logical pixels of the area (default: its centre). |
| `pan` | `dx dy` | Move what is shown by this many logical pixels (positive: towards right/bottom of the document). |
| `rotate` | `by` (`90` or `-90`) | Rotate the view, not the file. |
| `quit` | | Exit. |

Reserved for later versions: `page`.

## plugin → kiview

| Message | Keys | Meaning |
|---|---|---|
| `hello` | `version caps` | First message; `caps` is a comma list (`rotate`, later `pages`, `anim`, `vector`). |
| `opened` | `id w h pages anim format` | Opened (or shown again); natural size, e.g. `format=JPEG 4000×3000`. |
| `failed` | `id reason msg` | `reason` is `unsupported`, `corrupt`, `toolarge` or `io`. |
| `view` | `id zoom fit scroll_x scroll_y page pages` | State after every change. `zoom` is 1.0 for one document pixel per logical pixel; `fit` is 1 in a fit mode; `scroll_x/y` tell whether the document overflows the area on that axis (the wheel scrolls instead of changing page). |
| `anim_loop` | `id` | The animation completed a turn (a slideshow waits for it before moving on). |
| `frame` | `seq` | Drawn for that `surface` (contents first, kiview's announcement after). |

## Ordering

kiview sends `hello`, `surface` and `style` before the first `open`, and `surface` again
whenever the area or the density changes. The plugin may be sent `open` for a new
document while another is shown; the old one is released.
