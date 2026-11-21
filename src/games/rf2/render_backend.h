#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include "../../quad/offaxis_focus.h"
#include "atlas_fit.h"
namespace simqvfr::games::rf2 {
constexpr UINT kSceneWidth=kAtlasWidth, kSceneHeight=kAtlasHeight, kSceneSamples=4;
constexpr DXGI_FORMAT kSceneFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
// The focus box's share of the eye's field of view per axis until the ini's focus_percent arrives (SessionBackend::set_focus_fraction).
constexpr double kDefaultFocusFraction=0.30;
// Where the two views of an eye pass sit in the 3184x3096 scene target (the "atlas"): the outer view at the top left, the
// focus view at the top right. Both are free choices of ours: the composition (compositor.h) reads them back and
// writes the finished eye over the whole resolved target, so the game's post-processing sees a normal eye image.
// The defaults are the layout verified first: outer 398x774, focus 1592x1548.
struct AtlasLayout {
    UINT outerW = 398, outerH = 774, focusW = 1592, focusH = 1548;
    UINT focus_x() const { return kSceneWidth - focusW; }
    bool operator==(const AtlasLayout& o) const {
        return outerW == o.outerW && outerH == o.outerH && focusW == o.focusW && focusH == o.focusH;
    }
};
// From the ini's sizes (layer/config.cpp views4_sizes), as shares of the native eye: `outerScale` the periphery's size
// per axis (periphery_scale), `focusW`/`focusH` the focus view's (focus density x the box's share of the field of view).
// The focus view comes first; the outer view takes the width left beside it, at the eye's aspect.
inline AtlasLayout make_atlas_layout(double outerScale, double focusW, double focusH) {
    const AtlasFit fit = fit_atlas(outerScale, focusW, focusH);  // atlas_fit.h: the rule, shared with the headset menu
    AtlasLayout l;
    l.focusW = fit.focusW;
    l.focusH = fit.focusH;
    l.outerW = fit.outerW;
    l.outerH = fit.outerH;
    return l;
}
inline bool is_scene_desc(const D3D11_TEXTURE2D_DESC& d){return d.Width==kSceneWidth&&d.Height==kSceneHeight&&d.Format==kSceneFormat&&d.SampleDesc.Count==kSceneSamples;}
inline bool is_scene_resource(ID3D11Resource* resource){
 if(!resource)return false; Microsoft::WRL::ComPtr<ID3D11Texture2D> t; if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&t))))return false;
 D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); return is_scene_desc(d);}
struct FocusMapping{float remap[2][4]{};float rect[2][4]{};};
// The focus box of `fractionX` x `fractionY` of each eye's field of view, centred on the eye's forward axis and moved
// `offsetYDegrees` up (+) or down (-) from it (the ini's fixed_offset_y), kept inside the eye (clamp_focus_centre).
inline bool calculate_focus_mapping(const float (&tanFov)[2][4],double fractionX,double fractionY,FocusMapping& out,double offsetYDegrees=0.0){
 FocusMapping next{}; for(int eye=0;eye<2;++eye){
  const double l=tanFov[eye][0],r=tanFov[eye][1],d=tanFov[eye][2],u=tanFov[eye][3],w=r-l,h=u-d;
  if(!(w>0.0)||!(h>0.0))return false; float ep[16]{};ep[0]=float(2.0/w);ep[5]=float(2.0/h);ep[8]=float(-(r+l)/w);ep[9]=float(-(u+d)/h);ep[10]=ep[11]=1.0f;
  double cx=0,cy=0;if(offsetYDegrees!=0.0)cy=std::tan((offsetYDegrees<-80.0?-80.0:offsetYDegrees>80.0?80.0:offsetYDegrees)*3.14159265358979/180.0);
  if(!simqvfr::clamp_focus_centre(ep,fractionX,fractionY,cx,cy))return false;
  simqvfr::FocusBox box;float fp[16]{};if(!simqvfr::focus_projection(ep,fractionX,fractionY,cx,cy,box,fp)||!box.valid)return false;
  const auto m=simqvfr::clip_remap(ep,fp);next.remap[eye][0]=float(m.a);next.remap[eye][1]=float(m.b);next.remap[eye][2]=float(m.c);next.remap[eye][3]=float(m.d);
  for(int i=0;i<4;++i)next.rect[eye][i]=float(box.rect[i]);}
 out=next;return true;}
}
