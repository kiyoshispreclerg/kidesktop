# xisguard translations

xisguard uses GNU gettext, the same as most Linux desktop software. Every
user-visible string in the source is wrapped in `_("...")` (see
[shared/xis_i18n.h](../../shared/xis_i18n.h), used by every KiDesktop
program this way); at runtime `setlocale()`+`bindtextdomain()` pick the
right translation for whatever language the user's session is already in
(`LANG`/`LC_MESSAGES`) -- there is no in-app language picker, and none is
needed.

Log lines (`log_msg()`/`log_filtered()`) are deliberately **not**
translated: they're an audit trail meant to be grep-able and shareable
across languages, same convention as most Linux daemons. Only what the
Allow/Deny/Trust dialog and `--help` show to the user is translatable.

This `po/` directory belongs to xisguard only. Each kidesktop program that
gains translatable strings gets its own `po/` the same way, so installing
one program never pulls in translations (or build dependencies) for the
others.

## Adding a new language

1. Make sure `po/xisguard.pot` is current: `make pot` (from `xisguard/`).
2. Create the new language's catalog from it:
   ```
   msginit --input=po/xisguard.pot --locale=pt_BR --output=po/pt_BR.po
   ```
   (`--locale` takes any gettext locale code: `pt_BR`, `es`, `de`, `ja`, ...
   -- match how the target language's own locale is spelled on Linux,
   `locale -a` lists what's installed.)
3. Translate `po/pt_BR.po`: every `msgstr ""` gets the translated text.
   Any gettext-aware editor works (Poedit, Lokalize, `emacs -m po-mode`,
   or just a text editor -- it's a plain text format).
4. Add the language code to `po/LINGUAS` (one per line).
5. `make mo` builds `po/pt_BR.mo`; `make install` installs it to
   `$(PREFIX)/share/locale/pt_BR/LC_MESSAGES/xisguard.mo`. To try it without
   installing: `LANGUAGE=pt_BR ./xisguard` (GNU gettext honors `LANGUAGE`
   even when `LANG`/`LC_MESSAGES` are something else, which is the
   quickest way to preview a translation against your own session).

## Updating an existing translation after the source strings change

```
make update-po
```

Regenerates `po/xisguard.pot` from the current source and merges it into
every `po/*.po` listed in `po/LINGUAS`: strings that changed become
"fuzzy" (kept as a starting point, flagged for review), new strings
appear untranslated, removed ones are dropped. Existing translations
that didn't change are left alone.

## Notes for translators

- `%s`/`%d`/etc. placeholders must appear in the translation exactly as
  many times as in the original -- `msgfmt --check` (part of `make mo`)
  catches a mismatch and fails the build rather than shipping a crash.
- A few strings are format templates rather than whole sentences (e.g.
  the "Permission"/"Program" labels that go into the dialog's `%s: %s
  (%s)` lines) -- word order across the pieces may not translate cleanly
  into every language; flag these rather than guessing if that happens,
  so the code can be restructured into one full translatable sentence
  instead.
- Button labels (`"Allow"`, `"Deny (session)"`, ...) are embedded inside
  single-quoted shell arguments when building the zenity/xisserve dialog
  command -- **do not use an apostrophe** in a translated button label,
  it would break the command. Multi-word labels are fine.
- Comments starting `TRANSLATORS:` right above a `msgid` in the `.pot`
  (carried into each `.po` by `msgmerge`) exist specifically to explain
  context a bare string can't -- e.g. which config file a field maps to,
  or what a keyboard shortcut spec's syntax is.
