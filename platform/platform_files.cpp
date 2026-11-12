#include "platform_files.h"

#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

void platform_prepare_files() {
    const char* base = SDL_GetBasePath();   // the app bundle (with a trailing /)
    std::string game = std::string(base ? base : "./") + "game";
    struct stat st;
    if (stat(game.c_str(), &st) != 0) {   // no bundle (the Linux check build): the current folder as it is
        printf("[files] no %s: using the current folder\n", game.c_str());
        return;
    }
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    std::string work = docs ? docs : "";
    if (work.empty()) {
        char* pref = SDL_GetPrefPath("snicolast", "SegaRally2");
        if (pref) work = pref, SDL_free(pref);
    }
    if (work.empty() || chdir(work.c_str()) != 0) {
        fprintf(stderr, "[files] no writable folder; running from the bundle\n");
        chdir(game.c_str());
        return;
    }
    for (const char* d : {"save", "out", "data", "third_party"}) mkdir(d, 0755);
    // The bundle's folders, linked again at every launch: the bundle moves when
    // the app is reinstalled or updated
    auto link_to = [&](const std::string& from, const std::string& to) {
        unlink(to.c_str());
        if (symlink(from.c_str(), to.c_str()) != 0) perror(("[files] link " + to).c_str());
    };
    link_to(game + "/res", "res");
    link_to(game + "/textures", "textures");
    link_to(game + "/third_party/fonts", "third_party/fonts");
    std::string zip = game + "/srally2.zip";
    if (stat(zip.c_str(), &st) == 0) setenv("SR2_ROM_ZIP", zip.c_str(), 0);
    printf("[files] working folder %s, game files %s\n", work.c_str(), game.c_str());
}
