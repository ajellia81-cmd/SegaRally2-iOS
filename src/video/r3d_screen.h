// Screen-space heuristics applied to a built scene: geometry the game draws
// over the 3D image that a widescreen window treats differently.
#pragma once
#include "r3d_scene.h"

// Collapses the black cinematic bars the game draws over the scene
void r3d_strip_letterbox(R3DScene& out);
// Flat polygons filling a whole overlay viewport (the red flash of a hit)
// cover the 4:3 screen only: widened to the whole widescreen view
void r3d_widen_screen_fills(R3DScene& out, float wide);
