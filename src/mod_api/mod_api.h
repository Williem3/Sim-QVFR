/* Sim-QVFR's game-mod ABI (docs/architecture/mod-manager-plan.md): the one contract between the core (sim-qvfr.dll,
 * the OpenXR layer) and a game mod (sim-qvfr-<game>.dll), first-party or not.
 *
 * Plain C on purpose: no C++ types, no STL, no OpenXR or Direct3D headers, so a mod built with any compiler and any
 * runtime library talks to any core of the same ABI. Two tables cross the boundary, each starting with its own size
 * and the ABI version it was built against:
 *
 *   SqvfrHost  core -> mod: the services a mod may use (log, its ini, hooks, code patches)
 *   SqvfrMod   mod -> core: what the core asks of a game (src/games/adapter.h GameAdapter, entry for entry)
 *
 * The one export of a mod DLL is sqvfr_mod_entry (SqvfrModEntry): the core calls it once, with its host table, and the
 * mod returns its table (it lives for the process).
 *
 * Growing the ABI without breaking it: new entries are only ever appended to a table, and each side reads an entry only
 * when the other side's `size` covers it. A core reading a mod's table treats an entry beyond its size, or null, as the
 * GameAdapter default; a mod must do the same with the host's. A change that can't be an append (an entry's meaning,
 * a struct's layout) is a new SQVFR_ABI_VERSION, and the core refuses a table of another version.
 *
 * The structs below are frozen for an ABI version: a new field means a new struct and a new entry taking it.
 * Booleans cross as int32_t (0 false, 1 true). A query's int32_t result below 0 means "the core's default" (what
 * GameAdapter's own implementation answers), so a mod answers only what it knows. Strings are UTF-16 (wchar_t on
 * Windows) or UTF-8 (char), owned by the side that passes them and valid only during the call.
 */
#ifndef SIM_QVFR_MOD_API_H
#define SIM_QVFR_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQVFR_ABI_VERSION 1u

/* The export a mod DLL provides, by this name. */
#define SQVFR_MOD_ENTRY_NAME "sqvfr_mod_entry"

#define SQVFR_FOCUS_POINTS_MAX 10 /* quad/focus_shape.h kFocusPointsMax */

/* ---- Plain data, copied field by field from and to the core's own types ---- */

/* quad/views4_layout.h Views4Layout: where the four views sit in the game's target. */
typedef struct SqvfrLayout {
    double outer, focus, focusHeight; /* linear scale of the native eye width / height */
    double cropWidth, cropHeight;     /* the outer views' kept share of the eye (FOV crop) */
    double width[4];                  /* fraction of the target width */
    double height[4];                 /* fraction of the target height */
    double layerFloor;                /* one view size: every view at least this large, in native eye sizes */
    int32_t live;                     /* width/height come from a live layout */
    int32_t sharedColumnWidth;        /* eye columns: every view as wide as the outer view */
    int32_t secondViewRoom;           /* room for the focus view in its eye's image (multi-view) */
    int32_t secondViewBeside;         /* ... beside the eye rather than below */
} SqvfrLayout;

/* adapter.h QvfrStart: QVFR's launch settings, all known at xrCreateInstance. */
typedef struct SqvfrQvfrStart {
    SqvfrLayout layout;
    double focusFraction;  /* the focus box: share of each eye's tangent width (0: the eyes' own cameras) */
    double focusFractionY; /* and height (<= 0: as focusFraction) */
    int32_t dlssLive;      /* live resolution resizes the views below DLSS */
    int32_t focusCull;     /* the focus views skip draws outside their frustum */
    double focusCullScale; /* the cull box's extent, a multiple of the focus box */
    int32_t focusCullGaze; /* the cull box follows the gaze */
    const wchar_t* ini;    /* the launch's ini (the game's own [experiment] keys) */
} SqvfrQvfrStart;

/* adapter.h Views4Frame: what one game frame's views were rendered with. */
typedef struct SqvfrViews4Frame {
    float outerTan[2][4]; /* per eye: left, right, up, down tangents */
    float focusTan[2][4];
    float turn[2][4];     /* per eye: the focus camera's turn, a quaternion x, y, z, w */
    int32_t live;
    double width[4], height[4];             /* the view layout in the game's output */
    double renderWidth[4], renderHeight[4]; /* the views rendered (DLSS's input; else the same) */
    int32_t eyesOnly;                       /* the game drew only its two eyes this frame */
} SqvfrViews4Frame;

/* quad/focus_mask.h FocusArea: the focus shape as the compositor composes it. */
typedef struct SqvfrFocusArea {
    float point[2 * SQVFR_FOCUS_POINTS_MAX];
    int32_t points;
    float radius;
    float aspect;
    float visible[4]; /* left, top, right, bottom shares of the box the headset shows */
} SqvfrFocusArea;

/* engine/dlss_settings.h DlssSettings: Sim-QVFR's own (side-loaded) DLSS. */
typedef struct SqvfrDlssSettings {
    int32_t on;
    const wchar_t* folder; /* empty: the nvngx_dlss.dll Sim-QVFR includes */
    int32_t focusScale, focusPreset, eyePreset, textureSharpening;
    int32_t carVectors;
    int32_t upscaler; /* 0 DLSS, 1 FSR */
} SqvfrDlssSettings;

/* engine/nr_settings.h NrSettings: DLSS-NR on Sim-QVFR's own DLSS views. */
typedef struct SqvfrNrSettings {
    int32_t on;
    const wchar_t* folder;
    int32_t preset, style, intensity, localTone, localStructure;
    int32_t autoMask, periphery;
    int32_t focusScale, peripheryScale, workingLimit, peripheryCoverage, edgeBlend;
    int32_t beforeDlss;
    int32_t exposure;
    int32_t inputGamma, autoExposure;
    int32_t maxGain;
    int32_t saveFrames;
} SqvfrNrSettings;

/* adapter.h GameAdapter::LiveView: the headset menu's sizes, live, for a game composing its own box. */
typedef struct SqvfrLiveView {
    double periphery, focusX, focusY, density, feather, cornerRadius;
    int32_t border;
    float borderColor[4];
    int32_t gaze;
} SqvfrLiveView;

/* OpenXR's XrView, without OpenXR's headers: the pose and the field of view. */
typedef struct SqvfrView {
    float orientation[4]; /* x, y, z, w */
    float position[3];
    float fov[4]; /* angleLeft, angleRight, angleUp, angleDown (radians) */
} SqvfrView;

/* adapter.h QvfrMultiView: how the focus views were drawn (the overlay's badge). */
enum {
    SQVFR_MULTI_VIEW_NONE = 0,
    SQVFR_MULTI_VIEW_NVIDIA = 1,
    SQVFR_MULTI_VIEW_SOFTWARE = 2,
    SQVFR_MULTI_VIEW_FALLBACK = 3,
    SQVFR_MULTI_VIEW_EYES_ONLY = 4
};

enum { SQVFR_LOG_INFO = 0, SQVFR_LOG_DEBUG = 1 };

/* SqvfrHost::flags */
#define SQVFR_HOST_DEV_BUILD 0x1u /* a development build of the core (diagnostic switches allowed) */

/* engine/context_hooks.h Entry, as bits for SqvfrHost::d3d11_hooks_install's `wanted` (1 << entry). */
enum {
    SQVFR_D3D11_OM_SET_RENDER_TARGETS = 0,
    SQVFR_D3D11_OM_SET_RENDER_TARGETS_AND_UAVS = 1,
    SQVFR_D3D11_DRAW_INDEXED = 2,
    SQVFR_D3D11_DRAW = 3,
    SQVFR_D3D11_DRAW_INDEXED_INSTANCED = 4,
    SQVFR_D3D11_DRAW_INSTANCED = 5,
    SQVFR_D3D11_DRAW_INDEXED_INSTANCED_INDIRECT = 6,
    SQVFR_D3D11_DRAW_INSTANCED_INDIRECT = 7,
    SQVFR_D3D11_EXECUTE_COMMAND_LIST = 8,
    SQVFR_D3D11_RS_SET_VIEWPORTS = 9,
    SQVFR_D3D11_RS_SET_SCISSOR_RECTS = 10,
    SQVFR_D3D11_VS_SET_CONSTANT_BUFFERS1 = 11,
    SQVFR_D3D11_PS_SET_CONSTANT_BUFFERS1 = 12,
    SQVFR_D3D11_UNMAP = 13
};

/* context_hooks.h DrawCall. */
typedef struct SqvfrDrawCall {
    uint32_t count, instances, start, startInstance;
    int32_t baseVertex;
} SqvfrDrawCall;

/* context_hooks.h Observers, each null or a function of the mod's. D3D11 objects as opaque pointers (`context` an
 * ID3D11DeviceContext*, `rtvs` ID3D11RenderTargetView* const*, `viewports` D3D11_VIEWPORT, `rects` D3D11_RECT,
 * `buffers` ID3D11Buffer*), entries as SQVFR_D3D11_*. Booleans returned as int32_t. */
typedef struct SqvfrD3D11Observers {
    void (*targets)(void* context, uint32_t count, void* const* rtvs, void* dsv);
    void (*draw)(void* context, int32_t entry, uint32_t count, uint32_t instances);
    void (*execute)(void* context, void* list);
    int32_t (*draw_replace)(void* context, int32_t entry, const SqvfrDrawCall* call);
    void (*draw_after)(void* context, int32_t entry, uint32_t count, uint32_t instances, uint32_t start);
    int32_t (*viewports)(void* context, uint32_t count, const void* in, void* out);
    int32_t (*scissors)(void* context, uint32_t count, const void* in, void* out);
    int32_t (*constant_buffers)(void* context, int32_t pixel, uint32_t start, uint32_t count, void* const* in,
                                void** out);
    void (*unmap)(void* context, void* resource, uint32_t subresource);
} SqvfrD3D11Observers;

/* engine/nvapi_stereo.h Support. */
typedef struct SqvfrNvapiSupport {
    int32_t loaded, singlePassStereo, xyzw, multiView;
    int32_t status;
} SqvfrNvapiSupport;

/* SqvfrHost::multi_view_shaders_option */
enum {
    SQVFR_MVS_POSITION_TWINS = 0,  /* mvr_probe_position_twins */
    SQVFR_MVS_OWN_PIXEL_LOADS = 1, /* mvr_probe_own_pixel_loads */
    SQVFR_MVS_TINT_ROWS = 2,       /* mvr_probe_tint_rows */
    SQVFR_MVS_CB_ROWS = 3          /* mvr_probe_cb_rows */
};
enum { SQVFR_MVS_BACKEND_NONE = 0, SQVFR_MVS_BACKEND_NVAPI = 1, SQVFR_MVS_BACKEND_INSTANCED = 2 };

/* quad/focus_images.h FocusImagesStart: what the core settles at the session's start, for a mod that renders the
 * focus views itself at the D3D12 level. `device` an ID3D12Device*, `queue` the ID3D12CommandQueue* that presents
 * (both null in `early`). Booleans as int32_t. Starts with its own size (append-only, like the tables). */
typedef struct SqvfrFocusImagesStart {
    uint32_t size;
    void* device;
    void* queue;
    const wchar_t* ini;  /* the launch's ini: the mod reads its own switches there */
    int32_t mode;        /* [experiment] replay_probe: 1/2 cost probes, 3 the focus views */
    int32_t compose;     /* the core composes the focus image with the game's eyes this launch */
    int32_t apiPath;     /* the game's QVFR by this path (the mod's own switches default on) */
    double boxFraction, boxFractionY; /* the focus box: half extent in each eye's NDC (Y 0: as X) */
    double pixelScale, pixelScaleY;   /* focus pixels per eye pixel (Y 0: as X) */
    int32_t post;        /* the focus views' own post (tonemap, bloom, fxaa): needed to compose */
    int32_t dlss;        /* the focus views through the mod's DLSS */
    double dlssScale;    /* their render share (1: DLAA) */
} SqvfrFocusImagesStart;

/* quad/focus_images.h FocusImage: the focus views' image as the core composes it. `image` an ID3D12Resource* (both
 * eyes side by side, after the post, the swapchain's format), `state` its D3D12_RESOURCE_STATES. */
typedef struct SqvfrFocusImage {
    void* image;
    int32_t state;
    double box, boxY;     /* the focus box's half extent in each eye's NDC */
    float offset[2][2];   /* each eye's box centre off its forward axis (the gaze this frame used) */
} SqvfrFocusImage;

/* quad/focus_images.h FocusDlssState: what DLSS runs for the eyes and the focus views (the headset menu's note). */
typedef struct SqvfrFocusDlssState {
    int32_t seen, forcedDlaa;
    int32_t gameQuality;  /* the in-game mode as the game asked (NGX's PerfQuality value) */
    uint32_t width, height, outWidth, outHeight;  /* the eyes' feature as made */
    int32_t eyePreset, focusPreset;
    int32_t focusRunning;
} SqvfrFocusDlssState;

/* quad/focus_images.h FocusImages: a D3D12 mod's own focus views (SqvfrMod::focus_images). Every entry takes the
 * table's `self`; read each only where `size` covers it (null: not offered). Booleans as int32_t. */
typedef struct SqvfrFocusImages {
    uint32_t size; /* sizeof(SqvfrFocusImages) as the mod was built */
    void* self;
    /* xrCreateInstance, before the game makes its device, swap chain or renderer (no device or queue). */
    void (*early)(void* self, const SqvfrFocusImagesStart* start);
    int32_t (*start)(void* self, const SqvfrFocusImagesStart* start);
    void (*frame_end)(void* self);
    /* Where each eye's focus box sits, off its forward axis in the eye's NDC (x right, y up), for the next frame. */
    void (*set_gaze)(void* self, const float ndc[2][2]);
    /* Live: the output size as a share of the eye each way and the render share (0: unchanged). */
    void (*set_size)(void* self, double k, double kY, double renderScale);
    void (*set_box)(void* self, double box, double boxY);
    void (*set_eye_scale)(void* self, double scale);  /* the live periphery: the game's eyes at `scale` (1: off) */
    void (*set_stock)(void* self, int32_t on);        /* no focus views, the game's eyes at launch size */
    void (*set_eye_texture_match)(void* self, double share, double mips);
    int32_t (*focus_image)(void* self, SqvfrFocusImage* out);  /* 0 before the first focus image */
    int32_t (*eye_region)(void* self, float* fx, float* fy);   /* the share of each eye's image in use */
    void (*eye_allow)(void* self, int32_t composed);
    void (*gpu_scope)(void* self, int32_t compositor);
    int32_t (*census)(void* self, uint32_t frames);           /* development builds: the draw census */
    void (*set_live_diag)(void* self, uint32_t bits);
    /* DLSS: the eyes' and the focus views' presets (0: the game's own), at launch before `early` and live. */
    void (*set_dlss_presets)(void* self, int32_t eye, int32_t focus);
    int32_t (*dlss_state)(void* self, SqvfrFocusDlssState* out);  /* 0: nothing to report */
} SqvfrFocusImages;

/* ---- core -> mod ---- */

typedef struct SqvfrHost {
    uint32_t size; /* sizeof(SqvfrHost) as the core was built */
    uint32_t abi;  /* SQVFR_ABI_VERSION as the core was built */
    uint32_t flags;
    /* The game this process is (its id, "ats"), as the core recognised it from the exe. A mod serving several games
     * (one engine, several identities: SCS Software's two truck sims) picks the one it is in from this, and returns
     * that game's table. */
    const char* gameId;

    /* A line into the game's session log (UTF-8, no newline). SQVFR_LOG_DEBUG lines only in a Detailed logs launch. */
    void (*log)(int32_t level, const char* message);
    /* The launch's ini (the game's settings copy for this run); never null, empty when unknown. */
    const wchar_t* (*ini_path)(void);
    /* The folder for the game's captures (diagnostic images); never null. */
    const wchar_t* (*captures_dir)(void);

    /* Function hooks, shared with the core's (one hook library per process): `detour` replaces `target`, and
     * `*original` receives the trampoline that calls the target as it was. 1 on success (also when the target was
     * already hooked by this caller). */
    int32_t (*hook)(void* target, void* detour, void** original);
    /* Turns a hook from `hook` off again (the detour stays allocated: a thread may still be inside it). */
    int32_t (*unhook)(void* target);

    /* Code patches on the game's exe (engine/code_patch.h), by RVA from its base. */
    uintptr_t (*module_base)(void);
    int32_t (*code_matches)(uintptr_t rva, const uint8_t* anchor, size_t size);
    int32_t (*code_write)(uintptr_t rva, const void* bytes, size_t size);
    int32_t (*code_redirect)(uintptr_t rva, const uint8_t* anchor, size_t size, const uint8_t* stub, size_t stubSize);
    uintptr_t (*vtable_slot)(uintptr_t slotRva);
    int32_t (*vtable_slot_replace)(uintptr_t slotRva, uintptr_t expectedRva, const void* replacement);
    /* `size` bytes at `address` are committed and readable. */
    int32_t (*readable)(const void* address, size_t size);

    /* Asks the game to close as its window's close button does (diagnostic runs); how many windows were asked. */
    int32_t (*request_game_exit)(const char* why);

    /* ---- Appended after the first slice (still ABI 1: read each only where `size` covers it) ---- */

    /* Debug lines are written this launch (a mod skips building costly ones otherwise). */
    int32_t (*log_debug_on)(void);
    /* The folder the game's session log is in (debug captures and dumps go beside it); never null. */
    const wchar_t* (*log_dir)(void);

    /* The game's D3D11 immediate context observed at the API level (engine/context_hooks.h): one set of hooks in the
     * process, shared by the core and every mod. `wanted`: bits of SQVFR_D3D11_*; `observers` is copied. 1: hooked. */
    int32_t (*d3d11_hooks_install)(void* context, uint32_t wanted, const struct SqvfrD3D11Observers* observers);
    void (*d3d11_hooks_rescan)(void);
    void (*d3d11_hooks_uninstall)(void);
    int32_t (*d3d11_hooks_installed)(void);
    /* Our own calls on the game's thread pass straight through while a bypass is held: 1 enters, 0 leaves (nested). */
    void (*d3d11_hooks_bypass)(int32_t enter);

    /* The core's multi-view shader service (every vertex shader given its twins at creation, the private-data GUIDs
     * the twins carry: games/lmu/mvr_probe.h). One per process. */
    int32_t (*multi_view_shaders_install)(const wchar_t* ini, int32_t attachTwins, int32_t nameShaders);
    void (*multi_view_shaders_device)(void* device);
    void (*multi_view_shaders_option)(int32_t option, int32_t on); /* SQVFR_MVS_* */
    void (*multi_view_shaders_frame_end)(void);
    int32_t (*multi_view_shaders_backend)(void); /* SQVFR_MVS_BACKEND_* */

    /* NVIDIA's multi-view through NVAPI (engine/nvapi_stereo.h): what the device supports, two views on a context. */
    void (*nvapi_query)(void* device, struct SqvfrNvapiSupport* support);
    int32_t (*nvapi_set_two_views)(void* context, int32_t on, int32_t independentMasks, uint32_t targetOffset);
    const char* (*nvapi_status_name)(int32_t status);

    /* NVIDIA Nsight Aftermath GPU crash dumps (probe/aftermath.h; dev builds with the SDK, else they do nothing). */
    int32_t (*aftermath_built)(void);
    int32_t (*aftermath_enable_dumps)(void);
    int32_t (*aftermath_attach)(void* device, void* context, int32_t shaderErrors);
    void (*aftermath_mark)(uint64_t code);
    int32_t (*aftermath_marking)(void);
    void (*aftermath_set_marker_names)(void (*name)(uint64_t code, char* out, unsigned size));
    void (*aftermath_after_removal)(unsigned waitMs);
    void (*aftermath_detach)(void);

    /* Called when the core's hang watch fires (probe/hang_watch.h), before it writes its dump. */
    void (*hang_watch_on_fire)(void (*fn)(void));

    /* The layer's switches the game shares (quad/showcase.h, eye_off.h, car_lighting.h). */
    int32_t (*stock_wanted)(void);
    void (*set_stock_active)(int32_t on);
    void (*note_scene_frame)(void);
    int32_t (*eye_off)(void);              /* -1 both eyes on, 0 the left off, 1 the right off */
    uint64_t (*car_lighting_bits)(void);   /* quad/car_lighting.h's packed settings */

    /* The mod's own settings (its settings.json, set on the launcher's Game tab). `modId`: the id of the mod this
     * table was given to (its mods\<id> folder); empty when the core loaded it some other way. */
    const char* modId;
    /* The value of setting `key` of this mod ([mod_<modId>] of sim-qvfr.ini; ASCII, as settings.json names its values:
     * "1"/"0" for a toggle, an option's value for a choice) into `out`, terminated. Its length, or -1 when it isn't
     * set (the mod then uses its own default), the key isn't a setting name, or `size` is too small. `host` is this
     * table. sqvfr::setting (mod.hpp) wraps it. */
    int32_t (*mod_setting)(const struct SqvfrHost* host, const char* key, char* out, uint32_t size);
} SqvfrHost;

/* ---- mod -> core ---- */

/* Every entry takes the table's `self` first. Comments name the GameAdapter method an entry mirrors (adapter.h has what
 * each one means); "(query)" marks an int32_t answer where below 0 is the core's default. Opaque pointers: `device` an
 * ID3D11Device*, `texture`/`eyes` ID3D11Texture2D*, `chain` an XrSwapchain, `enumerate` the next layer's
 * PFN_xrEnumerateSwapchainImages, `frameEnd` a const XrFrameEndInfo* (OpenXR's own C structs). */
typedef struct SqvfrMod {
    uint32_t size; /* sizeof(SqvfrMod) as the mod was built */
    uint32_t abi;  /* SQVFR_ABI_VERSION as the mod was built */
    void* self;    /* passed back as every entry's first argument */
    const char* gameId; /* the game this table is for (SqvfrHost::gameId): the core refuses a table for another */

    int32_t (*has_qvfr)(void* self);                                      /* (query) */
    void (*views_offered)(void* self, uint32_t width, uint32_t height);
    int32_t (*own_qvfr)(void* self, const wchar_t* ini);                  /* (query) */
    int32_t (*qvfr_composable)(void* self);                               /* (query) */
    int32_t (*qvfr_compose_d3d11)(void* self);                            /* (query) */
    int32_t (*loads_cheeky)(void* self);                                  /* (query) */
    int32_t (*qvfr_eye_columns)(void* self);                              /* (query) */
    int32_t (*qvfr_one_view_size)(void* self);                            /* (query) */
    int32_t (*qvfr_separate_views)(void* self);                           /* (query) */
    int32_t (*qvfr_eye_extent)(void* self, uint32_t width[2], uint32_t height[2]);
    void* (*qvfr_views_atlas)(void* self, void* const eyes[2], float uv[4][4]);
    int32_t (*attaches_action_sets)(void* self);                          /* (query) */
    uint32_t (*hang_watch_s)(void* self);                                 /* 0: the core's default */

    int32_t (*qvfr_start)(void* self, const SqvfrQvfrStart* start);
    void (*qvfr_set_gaze)(void* self, const float tan[2][2], const int32_t valid[2], int32_t gazeSample);
    void (*qvfr_set_live_layout)(void* self, const SqvfrLayout* live);
    int32_t (*qvfr_live_resolution)(void* self);                          /* (query) */
    void (*qvfr_set_live)(void* self, double focusFraction, double focusFractionY, double cullScale, int32_t gaze);
    void (*qvfr_set_focus_mask)(void* self, int32_t on, const SqvfrFocusArea* area, int32_t marginTexels);
    void (*qvfr_set_focus_visible)(void* self, const float visible[4]);
    void (*qvfr_set_focus_corridor)(void* self, int32_t on, int32_t widthM, int32_t heightDm, int32_t spreadDeg,
                                    int32_t blendDm, int32_t show);
    void (*qvfr_set_object_detail)(void* self, int32_t percent);
    void (*qvfr_set_dlss)(void* self, const SqvfrDlssSettings* settings);
    void (*qvfr_set_nr)(void* self, const SqvfrNrSettings* settings);
    int32_t (*qvfr_dlss_held)(void* self);
    void (*qvfr_target_sizes)(void* self, int32_t render[2], int32_t output[2]);
    int32_t (*qvfr_take_frame)(void* self, SqvfrViews4Frame* frame);
    int32_t (*qvfr_latest_frame)(void* self, SqvfrViews4Frame* frame);
    int32_t (*qvfr_copies_layout)(void* self, const wchar_t* ini);        /* (query) */
    double (*qvfr_fixation_m)(void* self);
    int32_t (*qvfr_wants_game_image)(void* self);                         /* (query) */
    void (*qvfr_game_image)(void* self, void* texture, const int32_t rects[4][4]);
    void (*qvfr_note_views)(void* self, const SqvfrView* views, uint32_t count);
    void (*qvfr_jitter_views)(void* self, SqvfrView* views, uint32_t count);
    void (*visibility_mask_taken)(void* self, uint32_t viewIndex);
    void (*qvfr_frame_end)(void* self);
    int32_t (*qvfr_multi_view)(void* self);                               /* SQVFR_MULTI_VIEW_* */
    int32_t (*qvfr_car_light_costs)(void* self, double* gpuMs, double* cpuMs);

    void (*early_start)(void* self, const wchar_t* ini);
    void (*instance_created)(void* self, const wchar_t* ini, const wchar_t* mode);
    void (*session_created)(void* self, int32_t probed);
    void (*d3d11_session)(void* self, void* device);
    int32_t (*observes_d3d11)(void* self);                                /* (query) */
    int32_t (*wants_gaze)(void* self);                                    /* (query) */
    int32_t (*shows_overlay)(void* self);                                 /* (query) */
    void (*live_view)(void* self, const SqvfrLiveView* view);
    void (*qvfr_set_look)(void* self, double feather, double cornerRadius, int32_t border, const float color[4]);
    void (*session_running)(void* self, int32_t running);
    void (*session_destroyed)(void* self);
    void (*frame_end)(void* self);
    void (*swapchain_acquired)(void* self, uint64_t chain, uint32_t index, void* enumerate);
    void (*swapchain_releasing)(void* self, uint64_t chain);
    void (*swapchain_destroyed)(void* self, uint64_t chain);
    /* Null: the frame as it was. */
    const void* (*adjust_frame_end)(void* self, const void* frameEnd, void* enumerate);
    /* A D3D12 game whose mod renders the focus views itself (the core composes them): its table, for the process.
     * Null: none (the core's own QVFR paths). */
    const SqvfrFocusImages* (*focus_images)(void* self);
} SqvfrMod;

/* The mod's one export (SQVFR_MOD_ENTRY_NAME). `host` lives for the process. Null: the mod refuses this core (it logs
 * why through host->log first). */
typedef const SqvfrMod* (*SqvfrModEntry)(const SqvfrHost* host);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SIM_QVFR_MOD_API_H */
