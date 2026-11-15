// Optional de-dithering of the 256-colour art: an edge-preserving (bilateral)
// smoothing filter that removes error-diffusion speckle in skies and flat
// areas while keeping outlines and text sharp.
#pragma once

#include "recomp/win16/gdi.hpp"

namespace win16 {

// Returns the filtered picture. Only the part that changed since the previous
// call is filtered again, so a still screen costs a compare.
const Surface& dedither(const Surface& src);

}  // namespace win16
