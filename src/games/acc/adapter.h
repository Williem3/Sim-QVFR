// Assetto Corsa Competizione's adapter (../adapter.h). QVFR stage gate: four views, views 2-3 copies of the
// eyes (views.h); the layer shows views 0-1, uncomposed (ACC runs D3D11 through OpenComposite, and the
// composition is D3D12 only so far). No Cheeky.
#pragma once

#include "../adapter.h"

namespace acc {

simqvfr::games::GameAdapter& adapter();

}  // namespace acc
