// The D3D11 interfaces' entry counts (object_vtable.h) from the SDK's C declarations: this file alone sees them.
#define CINTERFACE
#define D3D11_NO_HELPERS  // the C++ helpers call methods the C declarations don't have
#include <d3d11_4.h>

#include "object_vtable.h"

namespace simqvfr {

const std::size_t kD3D11DeviceSlots = sizeof(ID3D11Device5Vtbl) / sizeof(void*);
const std::size_t kD3D11ContextSlots = sizeof(ID3D11DeviceContext4Vtbl) / sizeof(void*);

}  // namespace simqvfr
