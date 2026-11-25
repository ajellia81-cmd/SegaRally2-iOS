// iOS port: where the game's files are. The app bundle holds what ships with
// it (res/, textures/, third_party/fonts, the ROM) under game/; the game reads
// and writes relative paths (save/, out/, data/), so the working folder is
// the app's Documents folder, with links to the bundle's read-only folders.
#pragma once
void platform_prepare_files();
