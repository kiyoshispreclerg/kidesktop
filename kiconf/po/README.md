# kiconf translations

kiconf uses GNU gettext, the same as most Linux desktop software. Every
user-visible string in the source is wrapped in `_("...")` (see
[i18n.h](../i18n.h)); at runtime `setlocale()`+`bindtextdomain()` pick the
right translation for whatever language the user's session is already in
(`LANG`/`LC_MESSAGES`) -- there is no in-app language picker, and none is
needed.

This `po/` directory belongs to kiconf only. Each kidesktop program that
gains translatable strings gets its own `po/` the same way, so installing
one program never pulls in translations (or build dependencies) for the
others.

## Adding a new language

1. Make sure `po/kiconf.pot` is current: `make pot` (from `kiconf/`).
2. Create the new language's catalog from it:
   ```
   msginit --input=po/kiconf.pot --locale=pt_BR --output=po/pt_BR.po
   ```
   (`--locale` takes any gettext locale code: `pt_BR`, `es`, `de`, `ja`, ...
   -- match how the target language's own locale is spelled on Linux,
   `locale -a` lists what's installed.)
3. Translate `po/pt_BR.po`: every `msgstr ""` gets the translated text.
   Any gettext-aware editor works (Poedit, Lokalize, `emacs -m po-mode`,
   or just a text editor -- it's a plain text format).
4. Add the language code to `po/LINGUAS` (one per line).
5. `make mo` builds `po/pt_BR.mo`; `make install` installs it to
   `$(PREFIX)/share/locale/pt_BR/LC_MESSAGES/kiconf.mo`. To try it without
   installing: `LANGUAGE=pt_BR ./kiconf` (GNU gettext honors `LANGUAGE`
   even when `LANG`/`LC_MESSAGES` are something else, which is the
   quickest way to preview a translation against your own session).

## Updating an existing translation after the source strings change

```
make update-po
```

Regenerates `po/kiconf.pot` from the current source and merges it into
every `po/*.po` listed in `po/LINGUAS`: strings that changed become
"fuzzy" (kept as a starting point, flagged for review), new strings
appear untranslated, removed ones are dropped. Existing translations
that didn't change are left alone.

## Notes for translators

- `%s`/`%d`/etc. placeholders must appear in the translation exactly as
  many times as in the original -- `msgfmt --check` (part of `make mo`)
  catches a mismatch and fails the build rather than shipping a crash.
- A few strings are format templates rather than whole sentences (e.g.
  `"kiwm: %s"` in the Atalhos tab, where `%s` is filled with another
  translated string already) -- word order across the two pieces may not
  translate cleanly into every language; flag these rather than guessing
  if that happens, so the code can be restructured into one full
  translatable sentence instead.
- Comments starting `TRANSLATORS:` right above a `msgid` in the `.pot`
  (carried into each `.po` by `msgmerge`) exist specifically to explain
  context a bare string can't -- e.g. which config file a field maps to,
  or what a keyboard shortcut spec's syntax is.
