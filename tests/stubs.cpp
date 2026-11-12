// What tilegen.cpp needs from the rest of the program and the tests do not
// exercise: no HD backdrops and no HD text, so compose() takes the plain paths.
#include "sky_pack.h"
#include "text_hd.h"

void SkyPack::frame_done() {}
const SkyPack::Picture* SkyPack::find(const uint8_t*, int, bool) { return nullptr; }
const TextHD::Glyph* TextHD::find(uint32_t, const uint8_t*) const { return nullptr; }
const TextHD::Glyph* TextHD::find8(uint32_t, const uint8_t*) const { return nullptr; }
