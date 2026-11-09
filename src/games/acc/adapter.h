// Assetto Corsa Competizione's adapter (../adapter.h): four views from UE 4.26's stereo device, the focus views a second
// instanced pair (views.h), their screen-space effects shared from their eyes (view_share.h), composed by the D3D11
// compositor from ACC's own image (eye columns), Cheeky's Foveated DLSS on ACC's DLSS; the copies and focus gates
// (gates.h). ACC runs D3D11 through OpenComposite.
#pragma once

#include "../adapter.h"

namespace acc {

simqvfr::games::GameAdapter& adapter();

}  // namespace acc
