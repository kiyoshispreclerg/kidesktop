# xispanel translations

xispanel uses GNU gettext, the same as most Linux desktop software. Every
user-visible string in the source is wrapped in `_("...")` (see
[shared/xis_i18n.h](../../shared/xis_i18n.h), used by every KiDesktop
program this way); at runtime `setlocale()`+`bindtextdomain()` pick the
right translation for whatever language the user's session is already in
(`LANG`/`LC_MESSAGES`) -- there is no in-app language picker, and none is
needed.

What's translatable: `--help`/error text, the window/tasklist context
menus (Minimize, Close, Pin, ...), tooltips (battery/volume/CPU-GPU
temperature/network device readouts), and the toasts widgets fire
(low battery, new/safe-to-remove storage device, audio output/input
switched). Icon ids (`"battery-low"`, `"drive-removable-media"`, ...)
and DBus/UPower/sysfs property names/values compared via `strcmp()`
(`"Charging"`, `"Battery"`, MPRIS's `"Playing"`, SNI's `"Menu"`
property, ...) are protocol, never translated -- only read, never
shown as-is to the user. `metric_label()` in widgets/monitor.c keeps
`"CPU"`/`"RAM"`/`"GPU"`/`"VRAM"`/`"Swap"` untranslated on purpose: they're
the same word in Portuguese computing jargon, so translating them would
just be a no-op with extra catalog entries.

This `po/` directory belongs to xispanel only. Each kidesktop program that
gains translatable strings gets its own `po/` the same way, so installing
one program never pulls in translations (or build dependencies) for the
others.

## Adding a new language

1. Make sure `po/xispanel.pot` is current: `make pot` (from `xispanel/`).
2. Create the new language's catalog from it:
   ```
   msginit --input=po/xispanel.pot --locale=pt_BR --output=po/pt_BR.po
   ```
   (`--locale` takes any gettext locale code: `pt_BR`, `es`, `de`, `ja`, ...
   -- match how the target language's own locale is spelled on Linux,
   `locale -a` lists what's installed.)
3. Translate `po/pt_BR.po`: every `msgstr ""` gets the translated text.
   Any gettext-aware editor works (Poedit, Lokalize, `emacs -m po-mode`,
   or just a text editor -- it's a plain text format).
4. Add the language code to `po/LINGUAS` (one per line).
5. `make mo` builds `po/pt_BR.mo`; `make install` installs it to
   `$(PREFIX)/share/locale/pt_BR/LC_MESSAGES/xispanel.mo`. To try it without
   installing: `LANGUAGE=pt_BR ./xispanel` (GNU gettext honors `LANGUAGE`
   even when `LANG`/`LC_MESSAGES` are something else, which is the
   quickest way to preview a translation against your own session).

## Updating an existing translation after the source strings change

```
make update-po
```

Regenerates `po/xispanel.pot` from the current source and merges it into
every `po/*.po` listed in `po/LINGUAS`: strings that changed become
"fuzzy" (kept as a starting point, flagged for review), new strings
appear untranslated, removed ones are dropped. Existing translations
that didn't change are left alone.

## Notes for translators

- `%s`/`%d`/etc. placeholders must appear in the translation exactly as
  many times as in the original -- `msgfmt --check` (part of `make mo`)
  catches a mismatch and fails the build rather than shipping a crash.
- Comments starting `TRANSLATORS:` right above a `msgid` in the `.pot`
  (carried into each `.po` by `msgmerge`) exist specifically to explain
  context a bare string can't -- e.g. which config file a field maps to,
  or what a keyboard shortcut spec's syntax is.
