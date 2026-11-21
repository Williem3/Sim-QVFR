#pragma once
#include <cstdint>
#include "../../log.h"
namespace simqvfr::games::rf2 {
class BackendStats {
public:
    void note_draw(){++draws_;} void note_doubled(){++doubled_;} void note_no_twin(){++noTwin_;} void note_unsupported(){++unsupported_;}
    void note_composite(bool ok){++compositeAttempts_; if(!ok)++compositeFailures_;}
    void log_cb13(unsigned pass,int eye,const float remap[4],const float rect[4]){
        if(cb13Logs_>=100)return; ++cb13Logs_;
        simqvfr::logf("RF2 CB13 APPLY pass=%u eye=%s cb={%.6f %.6f %.6f %.6f} rect={%.6f %.6f %.6f %.6f}",pass,eye==0?"LEFT":"RIGHT",remap[0],remap[1],remap[2],remap[3],rect[0],rect[1],rect[2],rect[3]);
    }
    void log_frame(unsigned long long frame,uint64_t left,uint64_t right) const{
        if(!frame||frame%300!=0)return;
        simqvfr::logf("rf2 COLORED-FRAME-B0: frame=%llu draws=%llu doubled=%llu noTwin=%llu unsupported=%llu compositeL/R=%llu/%llu attempts=%llu failures=%llu",frame,
          static_cast<unsigned long long>(draws_),static_cast<unsigned long long>(doubled_),static_cast<unsigned long long>(noTwin_),static_cast<unsigned long long>(unsupported_),
          static_cast<unsigned long long>(left),static_cast<unsigned long long>(right),static_cast<unsigned long long>(compositeAttempts_),static_cast<unsigned long long>(compositeFailures_));
    }
private:
    uint64_t draws_=0,doubled_=0,noTwin_=0,unsupported_=0,compositeAttempts_=0,compositeFailures_=0; unsigned cb13Logs_=0;
};
}
