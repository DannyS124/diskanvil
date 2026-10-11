# Contributing

PRs are welcome. If you change anything in `src/drives/udisks.cpp`, run the self-test first:
```
sudo ./build/diskforge-selftest
./build/diskforge-selftest --guard
```
If you change code that reads files or anything from a drive (add-ons, backups, rescue maps,
partition tables, names), run the crash tests too: `./build/diskforge-selftest --fuzz`.

Don't open block devices directly or run anything as root. Everything goes through udisks2.
DiskForge also never listens for other programs (no sockets, no D-Bus service of its own);
the release script stops if that changes.

Want to add something without touching the code? Make an add-on instead: see `docs/ADDONS.md`.

Contributions are GPL-3.0-or-later like the rest of the project.

## Translating
1. Add your language to the `qt_standard_project_setup` line in `CMakeLists.txt`, e.g.
   `qt_standard_project_setup(I18N_SOURCE_LANGUAGE en I18N_TRANSLATED_LANGUAGES de)`.
2. Run `cmake --build build --target update_translations`. This creates `i18n/diskforge_de.ts`.
3. Translate it with Qt Linguist (`linguist6 i18n/diskforge_de.ts`) and send a pull request.
