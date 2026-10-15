# Theme format fixtures

Each `<N>/<name>.ini` is a theme file of format N, written by the theme builder
(<https://scarletnx.github.io/theme/>); `<name>.json` is the theme the GUI reads it to, its id the file's name,
in the builder's names. `gui_theme_formats` (tests/gui_theme_formats.c) checks every field each JSON names, and the
site's tests read the same files with the builder. docs/GUI.md, "Theme formats", says what a format promises.

They are never changed. A GUI change that makes one read otherwise is the change to fix. A new format gets a new
folder; a new fixture's JSON comes from the GUI itself:

    snx-test-gui-theme-formats --print tests/gui_themes/1/<name>.ini <name> > tests/gui_themes/1/<name>.json
