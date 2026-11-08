// Offline check of the DLSS output-extent fix (games/ace/ngx_extent.h). The fake bag declares NVSDK_NGX_Parameter's
// virtual functions in nvsdk_ngx_defs.h's order, so MSVC lays its vtable out as NGX's: the accessors must land on
// Set(name, unsigned int) and Get(name, unsigned int*). Then ACE's views4 sequence from a user's run (2026-11-08): four
// features made through one bag (periphery 582x599, focus 1259x1294), evaluated with the bag still at the last one.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "games/ace/ngx_extent.h"

struct ID3D11Resource;
struct ID3D12Resource;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

enum NgxResult : int { kSuccess = 0x1, kFailNotFound = static_cast<int>(0xbad00002) };

// NVSDK_NGX_Parameter's interface, in its header's order; every overload but the unsigned int pair counts as wrong.
struct NgxParameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;
    virtual NgxResult Get(const char* name, unsigned long long* value) const = 0;
    virtual NgxResult Get(const char* name, float* value) const = 0;
    virtual NgxResult Get(const char* name, double* value) const = 0;
    virtual NgxResult Get(const char* name, unsigned int* value) const = 0;
    virtual NgxResult Get(const char* name, int* value) const = 0;
    virtual NgxResult Get(const char* name, ID3D11Resource** value) const = 0;
    virtual NgxResult Get(const char* name, ID3D12Resource** value) const = 0;
    virtual NgxResult Get(const char* name, void** value) const = 0;
    virtual void Reset() = 0;
};

struct FakeBag final : NgxParameter {
    std::map<std::string, unsigned> ui;
    mutable int setUi = 0, getUi = 0, wrong = 0;

    void Set(const char*, unsigned long long) override { ++wrong; }
    void Set(const char*, float) override { ++wrong; }
    void Set(const char*, double) override { ++wrong; }
    void Set(const char* name, unsigned int value) override {
        ++setUi;
        ui[name] = value;
    }
    void Set(const char*, int) override { ++wrong; }
    void Set(const char*, ID3D11Resource*) override { ++wrong; }
    void Set(const char*, ID3D12Resource*) override { ++wrong; }
    void Set(const char*, void*) override { ++wrong; }
    NgxResult Get(const char*, unsigned long long*) const override { return ++wrong, kFailNotFound; }
    NgxResult Get(const char*, float*) const override { return ++wrong, kFailNotFound; }
    NgxResult Get(const char*, double*) const override { return ++wrong, kFailNotFound; }
    NgxResult Get(const char* name, unsigned int* value) const override {
        ++getUi;
        const auto it = ui.find(name);
        if (it == ui.end())
            return kFailNotFound;
        *value = it->second;
        return kSuccess;
    }
    NgxResult Get(const char*, int*) const override { return ++wrong, kFailNotFound; }
    NgxResult Get(const char*, ID3D11Resource**) const override { return ++wrong, kFailNotFound; }
    NgxResult Get(const char*, ID3D12Resource**) const override { return ++wrong, kFailNotFound; }
    NgxResult Get(const char*, void**) const override { return ++wrong, kFailNotFound; }
    void Reset() override { ++wrong; }
};

}  // namespace

int main() {
    using namespace simqvfr;

    // The accessors reach the unsigned int overloads.
    {
        FakeBag bag;
        ngx_set_ui(&bag, "OutWidth", 582);
        check(bag.setUi == 1 && bag.ui["OutWidth"] == 582 && bag.wrong == 0, "ngx_set_ui calls Set(name, unsigned int)");
        unsigned v = 0;
        check(ngx_get_ui(&bag, "OutWidth", v) && v == 582 && bag.getUi == 1 && bag.wrong == 0,
              "ngx_get_ui calls Get(name, unsigned int*)");
        unsigned missing = 7;
        check(!ngx_get_ui(&bag, "OutHeight", missing) && missing == 7, "ngx_get_ui: an absent name is false, value kept");
    }

    // ACE's views4 run: features made through one bag, then evaluated with the bag at the last-made size.
    {
        FakeBag bag;
        NgxExtents extents;
        const unsigned size[4][2] = {{582, 599}, {582, 599}, {1259, 1294}, {1259, 1294}};
        int handles[4] = {};
        for (int v = 0; v < 4; ++v) {
            ngx_set_ui(&bag, "OutWidth", size[v][0]);  // ACE's create helper
            ngx_set_ui(&bag, "OutHeight", size[v][1]);
            unsigned w = 0, h = 0;  // createHook: read before the create call
            check(ngx_get_ui(&bag, "OutWidth", w) && ngx_get_ui(&bag, "OutHeight", h), "create: the bag holds the output");
            extents.record(&handles[v], w, h);
        }
        check(bag.ui["OutWidth"] == 1259 && bag.ui["OutHeight"] == 1294, "after the creates the bag holds the focus size");
        const bool corrected[4] = {true, false, true, false};  // views evaluated 0, 1, 2, 3
        for (int v = 0; v < 4; ++v) {
            unsigned w = 0, h = 0;
            const bool known = extents.find(&handles[v], w, h);
            const bool put = known && ngx_put_output_extent(&bag, w, h);
            char what[96];
            std::snprintf(what, sizeof(what), "evaluate view %d: OutWidth/OutHeight %ux%u (%s)", v, size[v][0], size[v][1],
                          corrected[v] ? "corrected" : "already right");
            check(known && put == corrected[v] && bag.ui["OutWidth"] == size[v][0] && bag.ui["OutHeight"] == size[v][1],
                  what);
        }
        check(bag.wrong == 0, "only the unsigned int accessors were used");
        int other = 0;
        unsigned w = 0, h = 0;
        check(!extents.find(&other, w, h), "a handle never made is not found");
    }

    // The table: a handle made again takes its new size; the oldest of 32 makes room.
    {
        NgxExtents extents;
        int handles[40] = {};
        extents.record(&handles[0], 100, 100);
        extents.record(&handles[0], 200, 150);
        unsigned w = 0, h = 0;
        check(extents.find(&handles[0], w, h) && w == 200 && h == 150, "a handle made again takes its new size");
        for (int i = 1; i < 40; ++i)
            extents.record(&handles[i], 10 + i, 20 + i);
        bool oldGone = true, newKept = true;
        for (int i = 0; i < 8; ++i)
            oldGone &= !extents.find(&handles[i], w, h);
        for (int i = 8; i < 40; ++i)
            newKept &= extents.find(&handles[i], w, h) && w == 10u + i && h == 20u + i;
        check(oldGone && newKept, "40 handles: the last 32 kept, the first 8 gone");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
