// Layout of the guest-visible Direct3D 9 objects (d3d9_null.cpp), shared with the layer that forwards
// the game's calls to the renderer (d3d9_live.cpp).
#pragma once
#include "win32_util.h"

namespace bo1 {

// Object layouts (after COM_DATA).
// Resources share one layout so that GetDesc / Lock code can be common.
enum : uint32_t {
    R_KIND = COM_DATA + 0,   // one of K_*
    R_FORMAT = COM_DATA + 4,
    R_USAGE = COM_DATA + 8,
    R_POOL = COM_DATA + 12,
    R_WIDTH = COM_DATA + 16, // buffers: length in bytes
    R_HEIGHT = COM_DATA + 20,
    R_DEPTH = COM_DATA + 24,
    R_LEVELS = COM_DATA + 28,
    R_FVF = COM_DATA + 32,
    R_MEM = COM_DATA + 36,     // lazily allocated backing store (surface / buffer / volume level)
    R_PARENT = COM_DATA + 40,  // surfaces: owning texture (0 for stand-alone)
    R_CHILDREN = COM_DATA + 44, // textures: guest array of level objects (surfaces / volumes), faces*levels entries
    R_SIZE = 48,
};
enum : uint32_t { K_SURFACE = 1, K_TEXTURE, K_CUBE, K_VOLUMETEX, K_VOLUME, K_VB, K_IB };

// Bytes of one mip level / slice of `fmt` and its row pitch (block rows for DXT formats).
uint32_t d3d9_format_bytes(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch);
// The level object (surface or volume) of a texture; 0 if out of range.
uint32_t d3d9_tex_child(uint32_t tex, uint32_t face, uint32_t level);
// Called once the interface vtables exist (first Direct3DCreate9): d3d9_live.cpp attaches its observers.
void d3d9_live_attach();

} // namespace bo1
