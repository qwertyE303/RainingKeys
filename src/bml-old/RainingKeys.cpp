// ===========================================================================
// RainingKeys - Key display for Ballance (LEGACY BML native mod, .bmod)
// ---------------------------------------------------------------------------
// KeyViewer-like display: one box per key; on press the box changes color and
// shrinks, and a colored band flows out of it. The band keeps drifting after
// release until it leaves the screen.
//
// This file is the legacy-loader sibling of ../RainingKeys.cpp and is generated
// from it (see tools/make_bml_old_source.ps1) so that the two stay diffable:
// the simulation, the layout maths and every single draw call are meant to be
// textually identical. Everything host specific sits behind one include:
//
//   <imgui.h>   ->  "rk_imgui_compat.hpp"
//
// That facade implements exactly the handful of ImGui calls this mod makes, on
// top of Virtools 2D entities -  the legacy loader has no ImGui whatsoever
// (BML 0.3.43 exports 2433 symbols, none of them ImGui; verified with dumpbin).
// Concretely it redirects:
//
//   ImGui::GetTime()          -> CKTimeManager::GetTime() * 0.001
//                                (that API reports MILLISECONDS; treating it as
//                                seconds makes every frame delta exceed the
//                                mod's 0.25s clamp and freezes all animation,
//                                which is what killed every rain band before)
//   GetIO().WantTextInput     -> InputHook::IsBlock()
//   GetMainViewport()         -> CKRenderContext::GetWidth/GetHeight()
//   GetForegroundDrawList()   -> a per-frame command buffer realised into a pool
//                                of CK2dEntity + CKMaterial quads and BGui::Text
//                                sprites, z-ordered by submission index so the
//                                painter's order of the three draw passes is
//                                preserved exactly
//
// Loader differences the facade cannot hide - and only these:
//   * entry point: the legacy loader has no unload path, so no BMLExit is
//     exported (it calls OnUnload and deletes the instance itself on shutdown)
//   * press counts live in ModLoader\Config (no trailing "s")
//   * the in-game message board draws straight into a CKSpriteText and does not
//     parse the ANSI colour codes, so its message is plain text
//
// COORDINATES: render-context pixels, origin at the top-left. 1 unit = 1 pixel.
// ===========================================================================

// Legacy BML declares the IMod string API with CKSTRING, which is plain
// `const char*` (virtools/CKTypes.h) - identical to BML+ - so the shared code
// needs no string translation at all.
#include <BML/BMLAll.h>
#include "rk_imgui_compat.hpp"


#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Local (host-independent) file access. BML+ only exports its BML_*Utf8 helpers
// from 0.3.9 onwards, and legacy BML never exported them at all, so the mod
// carries its own tiny implementation instead.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
constexpr int SLOT_CAPACITY = 24;      // compile-time capacity
constexpr int BAND_POOL_INITIAL = 8;

// Press / release animation duration in seconds. Fixed on purpose: the
// transition should feel immediate, so it is not exposed in the config.
constexpr float PRESS_DURATION = 0.04f;

// Width of the soft edge at the length limit, as a FIXED distance in layout
// units. The band is solid up to (limit - FADE_LENGTH), then alpha ramps to
// zero at the limit with a smoothstep curve (the same shape Unity uses for
// RectMask2D softness, which is what KeyViewer relies on).
//
// Deliberately NOT a fraction of RainLength. It used to be 6% of the length
// limit, which made the soft edge grow and shrink with a setting that says how
// long the band is, not how soft its end is; the two are independent now, so
// RainLength only decides where the solid part stops. Like every other layout
// distance this is multiplied by the global Main/Scale on the way to pixels,
// and by nothing else. 18 is what 6% of the old default RainLength (300) came
// to, so the default look is unchanged.
constexpr float FADE_LENGTH = 18.0f;

// Global KPS, following KeyViewer's KPSCalculator: the number is the sum of the
// presses of the last KPS_UPDATE_MS milliseconds, sliced into buckets whose
// width is the (configurable) tick period. KeyViewer's KPSUpdateRate defaults to
// 1000, i.e. a single one-second bucket, and KPS_UPDATE_MS is that window.
constexpr float KPS_UPDATE_MS = 1000.0f;
// Bucket ring capacity: the widest slice is 10ms (the KpsRefresh floor), so 100
// buckets cover the window; the headroom leaves room for that floor to change.
constexpr int KPS_RING_MAX = 128;

// Default layout of the first four slots: a D-pad style cross, expressed in
// LAYOUT units relative to the bottom-left corner of Key01's box (see the
// coordinate contract above DrawOverlay). The shape is exactly the cross the
// author settled on for a 1600x1200 window - Key02 sits one spacing above and
// right of Key01, and so on - but the coordinates are now relative, so they -
// and the gaps between the boxes - follow Main/Scale.
constexpr int   DEFAULT_KEY_COUNT = 4;
constexpr float DEFAULT_KEY_LX[4] = { 0.0f, 100.0f, 200.0f, 100.0f };
constexpr float DEFAULT_KEY_LY[4] = { 0.0f, 100.0f,   0.0f,   0.0f };
constexpr float DEFAULT_KEY_W = 90.0f;
constexpr float DEFAULT_KEY_H = 90.0f;
// Band start offset for the up key: it flows away from the rest of the cross,
// so a small offset is all it needs.
constexpr float DEFAULT_RAIN_OFFSET_Y_UP = 10.0f;
// Band start offset for every other direction. Their bands would otherwise run
// straight through a neighbouring key box (the cross is only 100px apart), so
// the origin is pushed back far enough to clear it.
constexpr float DEFAULT_RAIN_OFFSET_Y = 110.0f;
// The Down key keeps the original blue band (it is the only arrow whose band
// flows downwards, passing through its own box); the other keys use white.
constexpr const char *DEFAULT_RAIN_COLOR = "255,255,255,200";
constexpr const char *DEFAULT_RAIN_COLOR_DOWN = "51,136,255,200";

// Spacing of the generated default layout, in LAYOUT units: it is exactly the
// old 100 screen pixels at Scale 100, and therefore halves at Scale 50.
constexpr float DEFAULT_KEY_SPACING = 100.0f;

// Where a fresh config puts the layout: Main/OffsetX/Y, i.e. the screen
// position of the bottom-left corner of Key01's box. Because everything else is
// derived from that corner, these two numbers are all a new config needs to
// place the whole display.
//
// The values reproduce the old hard-coded default placement (the cross sitting
// in the bottom third of a 1600x1200 window): OffsetY = 110 keeps the top of
// the cross 110px below the top edge, exactly as the previous defaults did.
constexpr float DEFAULT_LAYOUT_CORNER_X = 110.0f;
constexpr float DEFAULT_LAYOUT_CORNER_Y = 110.0f;

// Box size of the KPS / Total readouts, and where they sit relative to the key
// cross: one row below the bottom key row, KPS aligned with the left key.
// DY is a layout distance, so it scales together with the key spacing.
constexpr float DEFAULT_READOUT_W = 120.0f;
constexpr float DEFAULT_READOUT_H = 70.0f;
constexpr float DEFAULT_READOUT_DY = 100.0f;      // rows below the key row
// Gap used to derive the Total default X: one key spacing minus one box width.
constexpr float DEFAULT_LAYOUT_EDGE_GAP = DEFAULT_KEY_SPACING - DEFAULT_KEY_W;  // 10px

// Roundness of the key box, expressed as a fraction of its shorter side rather
// than in pixels, so it stays visually right when the box is resized or the
// whole display is scaled. 0.1111 * 90px reproduces the 10px look at the
// default box size (which is the 1600x1200 appearance).
constexpr float ROUND_RATIO = 0.1111f;

// Outline thickness in pixels at 100% scale (scaled with the display).
constexpr float OUTLINE_WIDTH = 2.0f;

// Hard cap on simultaneously live bands per key. The pool grows on demand up to
// this many entries instead of recycling the oldest one.
constexpr int BAND_POOL_MAX = 255;

// Count text sits at the bottom inside the box, at a fixed fraction of the box
// height. Not configurable: it must not drift when the display is scaled.
constexpr float COUNT_BOTTOM_FRACTION = 0.22f;

// Default text sizes (before the per-key LabelSize multiplier and Scale).
constexpr float BASE_LABEL_SIZE = 26.0f;
constexpr float COUNT_SIZE_RATIO = 0.75f;   // count text relative to the BASE label size; never scaled by LabelSize

// ---------------------------------------------------------------------------
// Host-independent file access
//
// These replace BML+'s BML_*Utf8 helpers. Those are only exported from BML+
// 0.3.9 onwards (and never by legacy BML), so using them would pin the mod to
// one loader generation. Paths are plain ANSI already (the game lives in an
// ASCII path), matching what the previous helpers did.
// ---------------------------------------------------------------------------
inline bool LocalFileExists(const char *path) {
    const DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Returns the number of bytes written, or -1 on failure. Callers here simply
// re-check with LocalFileExists, mirroring the old behaviour.
inline int LocalWriteTextFile(const char *path, const char *content) {
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr) return -1;
    const size_t len = (content != nullptr) ? std::strlen(content) : 0;
    const size_t wrote = (len > 0) ? std::fwrite(content, 1, len, f) : 0;
    std::fclose(f);
    return (wrote == len) ? static_cast<int>(wrote) : -1;
}

// Reads a whole file into a std::string. Returns false when it cannot be read.
inline bool LocalReadTextFile(const char *path, std::string &out) {
    out.clear();
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, got);
    }
    std::fclose(f);
    return true;
}

// Full path of the running executable, or an empty string on failure.
inline bool LocalExecutablePath(char *out, size_t outSize) {
    if (out == nullptr || outSize == 0) return false;
    out[0] = '\0';
    const DWORD n = GetModuleFileNameA(nullptr, out, static_cast<DWORD>(outSize));
    if (n == 0 || n >= outSize) {
        out[0] = '\0';
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Math / color helpers
// ---------------------------------------------------------------------------
inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline ImU32 PackColor(int r, int g, int b, int a) {
    return IM_COL32(Clampi(r, 0, 255), Clampi(g, 0, 255), Clampi(b, 0, 255), Clampi(a, 0, 255));
}

// Parse "R,G,B" or "R,G,B,A". The alpha component is optional and falls back to
// defA when omitted. All components are clamped to 0-255.
bool ParseColor(const char *text, int defR, int defG, int defB, int defA, int &outR, int &outG, int &outB, int &outA) {
    outR = defR; outG = defG; outB = defB; outA = defA;
    if (text == nullptr) return false;

    int r = 0, g = 0, b = 0, a = defA;
    const int got = std::sscanf(text, "%d , %d , %d , %d", &r, &g, &b, &a);
    if (got < 3) return false;

    outR = Clampi(r, 0, 255);
    outG = Clampi(g, 0, 255);
    outB = Clampi(b, 0, 255);
    outA = Clampi(a, 0, 255);
    return true;
}

inline float Progress(float t, float dur) {
    if (dur <= 0.0f) return 1.0f;
    const float k = t / dur;
    return k <= 0.0f ? 0.0f : (k >= 1.0f ? 1.0f : k);
}

// ---------------------------------------------------------------------------
// Easing curves (names follow DG.Tweening Ease, which KeyViewer uses)
// ---------------------------------------------------------------------------
enum EaseKind {
    EASE_NONE = 0, EASE_LINEAR,
    EASE_IN_QUAD, EASE_OUT_QUAD, EASE_IN_OUT_QUAD,
    EASE_IN_CUBIC, EASE_OUT_CUBIC, EASE_IN_OUT_CUBIC,
    EASE_OUT_BACK, EASE_OUT_BOUNCE,
    EASE_COUNT
};

const char *EaseName(int k) {
    switch (k) {
        case EASE_NONE:         return "None";
        case EASE_LINEAR:       return "Linear";
        case EASE_IN_QUAD:      return "InQuad";
        case EASE_OUT_QUAD:     return "OutQuad";
        case EASE_IN_OUT_QUAD:  return "InOutQuad";
        case EASE_IN_CUBIC:     return "InCubic";
        case EASE_OUT_CUBIC:    return "OutCubic";
        case EASE_IN_OUT_CUBIC: return "InOutCubic";
        case EASE_OUT_BACK:     return "OutBack";
        case EASE_OUT_BOUNCE:   return "OutBounce";
        default:                return "OutQuad";
    }
}

int EaseFromName(const char *name) {
    if (name == nullptr) return EASE_OUT_QUAD;
    for (int i = 0; i < EASE_COUNT; ++i) {
        if (_stricmp(name, EaseName(i)) == 0) return i;
    }
    return EASE_OUT_QUAD;
}

float ApplyEase(int kind, float t) {
    t = Clampf(t, 0.0f, 1.0f);
    switch (kind) {
        case EASE_NONE:         return 1.0f;   // instant, matching KeyViewer's Ease.Unset
        case EASE_LINEAR:       return t;
        case EASE_IN_QUAD:      return t * t;
        case EASE_OUT_QUAD:     return 1.0f - (1.0f - t) * (1.0f - t);
        case EASE_IN_OUT_QUAD:  return t < 0.5f ? 2.0f * t * t
                                                : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        case EASE_IN_CUBIC:     return t * t * t;
        case EASE_OUT_CUBIC: {
            const float u = 1.0f - t;
            return 1.0f - u * u * u;
        }
        case EASE_IN_OUT_CUBIC: {
            if (t < 0.5f) return 4.0f * t * t * t;
            const float u = -2.0f * t + 2.0f;
            return 1.0f - (u * u * u) * 0.5f;
        }
        case EASE_OUT_BACK: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            const float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case EASE_OUT_BOUNCE: {
            constexpr float n1 = 7.5625f, d1 = 2.75f;
            if (t < 1.0f / d1)      return n1 * t * t;
            else if (t < 2.0f / d1) { const float u = t - 1.5f / d1;   return n1 * u * u + 0.75f; }
            else if (t < 2.5f / d1) { const float u = t - 2.25f / d1;  return n1 * u * u + 0.9375f; }
            else                    { const float u = t - 2.625f / d1; return n1 * u * u + 0.984375f; }
        }
        default:                return t;
    }
}

// ---------------------------------------------------------------------------
// Band direction
// ---------------------------------------------------------------------------
enum Dir { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

const char *DirName(int d) {
    switch (d) {
        case DIR_UP:    return "Up";
        case DIR_DOWN:  return "Down";
        case DIR_LEFT:  return "Left";
        case DIR_RIGHT: return "Right";
        default:        return "Up";
    }
}

int DirFromName(const char *name) {
    if (name == nullptr) return DIR_UP;
    if (_stricmp(name, "Up") == 0)    return DIR_UP;
    if (_stricmp(name, "Down") == 0)  return DIR_DOWN;
    if (_stricmp(name, "Left") == 0)  return DIR_LEFT;
    if (_stricmp(name, "Right") == 0) return DIR_RIGHT;
    return DIR_UP;
}

inline bool IsVertical(int d) { return d == DIR_UP || d == DIR_DOWN; }

// ---------------------------------------------------------------------------
// RGBA color
// ---------------------------------------------------------------------------
struct Color4 {
    int r = 255, g = 255, b = 255, a = 255;
};

inline Color4 LerpColor(const Color4 &x, const Color4 &y, float k) {
    k = Clampf(k, 0.0f, 1.0f);
    Color4 o;
    o.r = static_cast<int>(std::lround(x.r + (y.r - x.r) * k));
    o.g = static_cast<int>(std::lround(x.g + (y.g - x.g) * k));
    o.b = static_cast<int>(std::lround(x.b + (y.b - x.b) * k));
    o.a = static_cast<int>(std::lround(x.a + (y.a - x.a) * k));
    return o;
}

inline ImU32 ToU32(const Color4 &c) { return PackColor(c.r, c.g, c.b, c.a); }

// ---------------------------------------------------------------------------
// Bound config property
// ---------------------------------------------------------------------------
struct Prop {
    IProperty *p = nullptr;
    bool b = false;
    int i = 0;
    float f = 0.0f;
    char s[128] = {};

    bool GetBool() const { return p ? (p->GetBoolean() != 0) : b; }
    int GetInt() const { return p ? p->GetInteger() : i; }
    float GetFloat() const { return p ? p->GetFloat() : f; }
    const char *GetStr() const { return p ? p->GetString() : s; }
    CKKEYBOARD GetKey() const { return p ? p->GetKey() : static_cast<CKKEYBOARD>(i); }

    // Writes through to the config, falling back to the cached value when the
    // property could not be created.
    void SetFloat(float v) {
        f = v;
        if (p != nullptr) p->SetFloat(v);
    }
};

// ===========================================================================
// One key slot
// ===========================================================================
struct Slot {
    // ---- config properties ----
    Prop cfgKey, cfgLabel, cfgLabelSize, cfgLabelColor;
    Prop cfgX, cfgY, cfgW, cfgH;
    Prop cfgBgColor, cfgPrColor, cfgPrScale;
    Prop cfgEnableRaining, cfgRainDir, cfgRainOffX, cfgRainOffY;
    Prop cfgRainSpeed, cfgRainLength, cfgRainWidth, cfgRainColor;
    Prop cfgEnableCount, cfgCountSize, cfgCountColor;

    bool bound = false;          // config properties created for this slot

    // ---- runtime state, refreshed from config every frame ----
    int    keyCode = 0;
    bool   enabled = false;
    char   labelText[64] = {};             // the text actually drawn
    float  labelSize = 1.0f;
    Color4 labelColor{255, 255, 255, 255};
    float  cx = 0.0f, cy = 0.0f, w = 90.0f, h = 90.0f;
    Color4 bgNormal{26, 26, 26, 180};
    Color4 bgPressed{255, 255, 255, 230};
    float  pressScale = 0.9f;
    int    pressCurve = EASE_OUT_QUAD;

    bool   rainOn = true;
    int    rainDir = DIR_UP;
    float  rainOffX = 0.0f, rainOffY = 0.0f;
    float  rainSpeed = 300.0f, rainLen = 300.0f, rainWidth = 0.0f;
    Color4 rainColor{255, 255, 255, 200};

    // ---- press counter ----
    // Persisted in our own Configs/RainingKeys.json, not in BML's .cfg: a count
    // needs no menu entry, and keeping it out of the config means clearing or
    // exporting counts never touches the user's settings.
    bool   countEnabled = false;
    long   count = 0;
    float  countSize = 1.0f;                // multiplier on the count text
    Color4 countColor{255, 255, 255, 255};

    // ---- key press counts, used by the global KPS readout ----
    // totalPresses is a monotonic per-key counter; the KPS update diffs it so a
    // press is never counted twice or missed, regardless of frame timing.
    long   totalPresses = 0;

    // ---- animation state ----
    bool   pressed = false;
    float  tAnim = 0.0f;
    float  fromScale = 1.0f;
    float  curScale = 1.0f;
    Color4 fromBg{26, 26, 26, 180};
    Color4 curBg{26, 26, 26, 180};

    // ---- band pool (grows on demand, up to BAND_POOL_MAX) ----
    // A band is a rectangle living in a fixed corridor above the key edge.
    // Coordinates are measured from the key edge, positive along the flow
    // direction. RainLength is the ceiling; alpha fades out towards it.
    //
    //   tail : lower edge of the band
    //   head : upper edge of the band
    //
    //   while held   : tail stays pinned at 0, head keeps growing (1x speed)
    //   after release: if head reached the ceiling, head freezes and tail
    //                  travels up to meet it (band is eaten from below);
    //                  otherwise head and tail drift up together.
    //
    // Both edges only ever move in the flow direction, so nothing ever
    // retreats. The band dies when tail catches up with head.
    struct Band {
        bool   alive = false;
        bool   held = false;       // button still down
        bool   drifting = false;   // released and head has not reached the ceiling yet
        float  startTime = 0.0f;
        float  head = 0.0f;
        float  tail = 0.0f;
        Color4 color{51, 136, 255, 220};   // frozen at spawn, never changes
    };
    std::vector<Band> bands;
};

// Draw-order record for bands: press time / slot / pool index
struct BandRef {
    float t = 0.0f;
    int slot = 0;
    int band = 0;
    BandRef(float t_, int s_, int b_) : t(t_), slot(s_), band(b_) {}
};

// ===========================================================================
// A readout slot: the global KPS / Total numbers. These have no key binding and
// no rain band - they are display-only, so they reuse the box look but only
// carry the properties that make sense for a number.
// ===========================================================================
struct ReadoutSlot {
    Prop cfgX, cfgY, cfgW, cfgH;
    Prop cfgBgColor;
    Prop cfgText, cfgLabelSize, cfgLabelColor;   // centred label, like Key's Label
    Prop cfgCountSize, cfgCountColor;            // value at the bottom, like Key's Count
    Prop cfgRefresh;                             // KPS only: the KPS tick period

    bool   bound = false;
    bool   visible = false;
    float  cx = 0.0f, cy = 0.0f, w = 90.0f, h = 90.0f;
    Color4 bg{18, 18, 18, 200};
    char   label[32] = {};               // e.g. "KPS"
    float  labelSize = 1.0f;
    Color4 labelColor{255, 255, 255, 255};
    float  valueSize = 1.0f;
    Color4 valueColor{255, 255, 255, 255};
    float  refresh = 1000.0f;    // KPS tick period in milliseconds

    // Runtime
    char   valueText[48] = {};  // the number, drawn at the bottom inside
    double cachedValue = 0.0;
};

// ===========================================================================
// The mod
// ===========================================================================

// Version gate. The legacy loader also compares BMLVersion{major,minor,build}
// and rejects a host older than the declared one, but here the declared version
// IS the build provenance: this target is compiled against the legacy BML
// 0.3.43 SDK (BuildVer.h) for the matching 0.3.43 runtime. The only wrinkle is
// the field name - legacy BMLVersion's third member is "build" while BML+
// 0.3.3+ calls it "patch" - and DECLARE_BML_VERSION absorbs that.

class RainingKeysMod final : public IMod {
public:
    explicit RainingKeysMod(IBML *bml) : IMod(bml) {}

    // --- metadata (all English) --------------------------------------------
    const char *GetID() override { return "RainingKeys"; }
    const char *GetVersion() override { return "0.1.1"; }
    const char *GetName() override { return "RainingKeys"; }
    const char *GetAuthor() override { return "Entity_303-E3"; }
    const char *GetDescription() override {
        return "Raining Keys for Ballance. ";
    }
    DECLARE_BML_VERSION;

    // -----------------------------------------------------------------------
    // Legacy-SDK workaround: two vtable slots that BML+ does not have.
    //
    // 0.3.34 added OnPhysicalize / OnUnphysicalize to IMod, growing its vtable
    // from 18 to 20 entries - which is exactly why the two loaders cannot share
    // one binary. IMod is declared __declspec(dllimport), so MSVC expects even
    // these inline-defaulted slots to come from BML.dll, and the import library
    // shipped with the 0.3.43 SDK predates them (it still has the 18-entry IMod).
    // Overriding the two slots with no-ops both satisfies the linker and states
    // plainly that this mod has nothing to do with physicalization.
    // -----------------------------------------------------------------------
    void OnPhysicalize(CK3dEntity*, CKBOOL, float, float, float, CKSTRING, CKBOOL, CKBOOL,
                       CKBOOL, float, float, CKSTRING, VxVector, int, CKMesh**, int, VxVector*,
                       float*, int, CKMesh**) override {}
    void OnUnphysicalize(CK3dEntity*) override {}

    // --- lifecycle ---------------------------------------------------------
    void OnLoad() override {
        // Bind the drawing facade to the host before anything can draw.
        ImGui::Compat::Init(m_bml, GetLogger());

        if (const ImGuiViewport *vp = ImGui::GetMainViewport()) {
            m_ViewportSize = vp->Size;
        }

        BindGlobalConfig();

        const int wanted = Clampi(m_CfgSlotCount.GetInt(), 1, SLOT_CAPACITY);
        EnsureSlotsBound(wanted);
        EnsureReadoutsBound();
        ReadConfigAll();
        ReadCounts();

        Log("[RainingKeys] loaded. BML SDK %s, drawing backend %s, slots=%d active=%d viewport=%.0fx%.0f",
            BML_VERSION, IMGUI_VERSION, m_BoundSlots, CountActive(),
            m_ViewportSize.x, m_ViewportSize.y);

        // Always-on self check: prints the resolved binding of every slot and
        // whether the input manager is usable, so a "keys do nothing" report can
        // be diagnosed from the log alone.
        {
            InputHook *input = m_bml ? m_bml->GetInputManager() : nullptr;
            Log("[RainingKeys] selfcheck: inputManager=%s", input ? "OK" : "NULL");
            for (int i = 0; i < m_BoundSlots; ++i) {
                const Slot &s = m_Slots[i];
                Log("[RainingKeys] selfcheck: slot%d key=%d enabled=%d pos=(%.0f,%.0f) size=%.0fx%.0f",
                    i + 1, s.keyCode, s.enabled ? 1 : 0, s.cx, s.cy, s.w, s.h);
            }
        }

        // Legacy-only: report what the readout binding decided, because the
        // config menu cannot show it (see this script's step 3b).
        if (m_CfgShowDebug.GetBool()) {
            Log("[RainingKeys] readouts: KPS=%s Total=%s%s",
                m_Kps.bound ? "bound" : "off", m_Total.bound ? "bound" : "off",
                (m_Kps.bound && m_Total.bound)
                    ? ""
                    : " (a category whose switch is enabled during this session appears"
                      " in the menu only after a restart: this loader builds the Mod"
                      " Options GUI once, at base.cmo load)");
        }

        RegisterCommands();

        // No "mod loaded" message on purpose: the in-game chat should stay clean.
    }

    // The counts used to be flushed only from OnUnload, which never runs here:
    // IMod::OnUnload is broadcast solely by ModLoader::Release(), whose only
    // trigger is DllMain(DLL_PROCESS_DETACH), and Ballance never gets there.
    // Two independent proofs from a real session: Release() logs "Releasing Mod
    // Loader" and "Goodbye!" unconditionally and neither line appears in the
    // log, and Release() also saves the config again, while RainingKeys.cfg's
    // timestamp stays at the moment the session *started*.
    //
    // The in-game exit message is delivered while the process is still fully
    // alive - the log shows it - so the counts are flushed there. That is also
    // the safer place for file I/O than a DLL detach handler.
    void OnExitGame() override {
        SaveCounts();
        Log("[RainingKeys] exit game; counts flushed");
    }

    void OnUnload() override {
        SaveCounts();
        Log("[RainingKeys] unloaded");
    }

    // --- per-frame simulation ---------------------------------------------
    void OnProcess() override {
        if (!m_bml) return;

        // Opens the drawing frame before any early return below, so a gated-off
        // frame still tells the backend to clear what was on screen. A retained
        // host keeps drawing whatever was last shown, so "draw nothing" has to
        // be said out loud. The matching EndFrame is at the end of DrawOverlay.
        ImGui::Compat::BeginFrame();

        const float now = static_cast<float>(ImGui::GetTime());
        float dt = (m_LastTime < 0.0f) ? 0.0f : (now - m_LastTime);
        m_LastTime = now;
        if (dt < 0.0f || dt > 0.25f) dt = 0.0f;
        m_TotalTime += dt;
        ++m_FrameCount;

        if (const ImGuiViewport *vp = ImGui::GetMainViewport()) {
            m_ViewportSize = vp->Size;
        }

        m_ShowEnabled = m_CfgEnabled.GetBool();
        m_ShowOnlyInLevel = m_CfgOnlyInLevel.GetBool();
        m_IsIngame = m_bml->IsIngame() != 0;

        // WantCaptureKeyboard is NOT a reliable "a text field is focused" test: on
        // some host builds it stays 1 all the time (observed on BML+ 0.3.13), which
        // made every key press get swallowed. WantTextInput is the flag that really
        // means "an input box would consume typed characters", so gate on that.
        const ImGuiIO &io = ImGui::GetIO();
        m_WantCaptureKeyboard = io.WantCaptureKeyboard ? true : false;
        m_WantTextInput = io.WantTextInput ? true : false;

        const bool debug = m_CfgShowDebug.GetBool();
        const bool gatedByEnabled = !m_ShowEnabled;
        const bool gatedByLevel = m_ShowOnlyInLevel && !m_IsIngame;

        if (debug && (m_FrameCount % 300) == 0) {
            Log("[RainingKeys] f=%u t=%.1f vp=%.0fx%.0f slots=%d active=%d enabled=%d inLevel=%d captureKB=%d textInput=%d gated=%s%s",
                m_FrameCount, m_TotalTime, m_ViewportSize.x, m_ViewportSize.y,
                m_BoundSlots, CountActive(),
                m_ShowEnabled ? 1 : 0, m_IsIngame ? 1 : 0,
                m_WantCaptureKeyboard ? 1 : 0, m_WantTextInput ? 1 : 0,
                gatedByEnabled ? "ENABLED " : "", gatedByLevel ? "LEVEL" : "");
        }

        if (gatedByEnabled || gatedByLevel) return;

        // Slot count may only shrink at runtime: properties for slots are created
        // once at load. BML offers no way to delete a config property, so creating
        // them on demand would leave orphaned menu entries behind forever.
        EnsureReadoutsBound();

        ReadConfigAll();
        ReadReadout(m_Kps, true, "KPS");
        ReadReadout(m_Total, false, "Total");
        UpdateReadouts(dt);

        InputHook *input = m_bml->GetInputManager();
        if (input == nullptr) return;

        // Do not react while the player is typing in a UI text field
        if (!m_WantTextInput) {
            SimulateKeys(input, dt);
        }

        // Draw during OnProcess, i.e. BEFORE BML calls ImGui::Render().
        // This is the only working draw path: drawing from OnRender happens after
        // ImGui::Render() and is discarded. See the P0 probe results in the header.
        DrawOverlay();
    }

    // --- OnRender: intentionally NOT used for drawing ----------------------
    // OnRender runs AFTER BML has already called ImGui::Render(), so anything
    // appended to a draw list there is never submitted (the probe build proved
    // this: the callbacks fire, but the geometry silently disappears).
    // All drawing therefore happens in OnProcess (see DrawOverlay below).
    void OnRender(CK_RENDER_FLAGS /*flags*/) override {
        ++m_RenderCount;
    }

    void OnModifyConfig(const char *category, const char *key, IProperty * /*prop*/) override {
        if (m_CfgShowDebug.GetBool()) {
            Log("[RainingKeys] config changed: %s / %s", category ? category : "?", key ? key : "?");
        }
    }

private:
    // =======================================================================
    // Key state simulation
    // =======================================================================
    void SimulateKeys(InputHook *input, float dt) {
        for (int i = 0; i < m_SlotCount; ++i) {
            Slot &s = m_Slots[i];
            if (!s.enabled || s.keyCode <= 0) continue;

            const bool down = input->IsKeyDown(static_cast<CKDWORD>(s.keyCode)) != 0;

            if (down != s.pressed) {
                s.fromScale = s.curScale;
                s.fromBg = s.curBg;
                s.tAnim = 0.0f;
                s.pressed = down;

                if (down) {
                    OnKeyPressed(s, m_TotalTime);
                } else {
                    ReleaseBands(s);
                }
            }

            AdvanceAnimation(s, dt);
            AdvanceBands(s, dt);
        }
    }

    // A press happened: bump the counter and start a band. The press itself is
    // counted for KPS by UpdateReadouts, which only needs the global total.
    void OnKeyPressed(Slot &s, float now) {
        if (s.countEnabled) ++s.count;
        ++s.totalPresses;

        SpawnBand(s, now);
    }

    // Global KPS, ported from KeyViewer's KPSCalculator.
    //
    // KeyViewer sums a sliding window of non-overlapping buckets instead of
    // decaying an accumulator:
    //
    //     every <update period>:  temp = pressCount; pressCount = 0;
    //                             kps = temp + (sum of the stored buckets)
    //                             store temp, keep 1000 / <period> of them
    //
    // The SUM SPAN is fixed at one second, exactly like KeyViewer (its
    // KPSUpdateRate is both the tick period and the bucket width). The value is
    // therefore an exact integer - 5 presses per second reads 5, not 4.8 - and
    // it returns to 0 by itself as soon as every press has left the window, with
    // no exponential tail and no hard cut-off to snap it there.
    //
    // KpsRefresh is the tick period, so it controls both how often the number is
    // recomputed AND how finely the second is sliced. At its 1000ms default there
    // is a single bucket, which is precisely KeyViewer's default behaviour; a
    // smaller value makes the number respond sooner while still measuring a full
    // second (at the cost of stepping in coarser units, since a bucket then holds
    // fewer presses).
    //
    // The bucket only has to be frame-rate independent: every frame the delta of
    // the global monotonic press total is folded in, so no press can be lost or
    // counted twice.
    //
    // All three of KeyViewer's outputs are kept (Kps / Max / Average); only Kps
    // is displayed for now. Like KeyViewer's Average, samples that are 0 are not
    // folded into the average.
    void UpdateReadouts(float dt) {
        if (m_Kps.bound) {
            long total = 0;
            for (int i = 0; i < m_SlotCount; ++i) {
                const Slot &s = m_Slots[i];
                if (s.enabled) total += s.totalPresses;
            }
            const long delta = total - m_LastPressTotal;
            m_LastPressTotal = total;
            if (delta > 0) m_KpsPressCount += static_cast<int>(delta);

            // KpsRefresh is the tick period (bounded to <= the 1s window so at
            // least one bucket always exists).
            const float tickMs = Clampf(m_Kps.refresh, 10.0f, KPS_UPDATE_MS);
            const int   buckets = Clampi(static_cast<int>(KPS_UPDATE_MS / tickMs), 1, KPS_RING_MAX);

            m_KpsElapsedMs += dt * 1000.0f;
            if (m_KpsElapsedMs >= tickMs) {
                // Restart the interval like KeyViewer's watch.Restart(), so a long
                // frame cannot queue up several ticks and double count.
                m_KpsElapsedMs = 0.0f;

                const int sample = m_KpsPressCount;
                m_KpsPressCount = 0;
                m_KpsRing[m_KpsRingHead] = sample;
                m_KpsRingHead = (m_KpsRingHead + 1) % KPS_RING_MAX;

                // Sum the buckets that span the last second, newest first.
                int kps = 0;
                for (int k = 0; k < buckets; ++k) {
                    int idx = m_KpsRingHead - 1 - k;
                    if (idx < 0) idx += KPS_RING_MAX;
                    kps += m_KpsRing[idx];
                }

                m_KpsCur = kps;
                if (m_KpsCur > m_KpsMax) m_KpsMax = m_KpsCur;
                if (m_KpsCur != 0) {
                    m_KpsAvgSum += static_cast<double>(m_KpsCur);
                    ++m_KpsAvgN;
                }

                // The number is rebuilt on every tick, so a smaller KpsRefresh is
                // what makes it respond sooner.
                m_Kps.cachedValue = static_cast<double>(m_KpsCur);
                FormatReadout(m_Kps);

                if (m_CfgShowDebug.GetBool()) {
                    Log("[RainingKeys] kps: tick=%.0fms sample=%d buckets=%d cur=%d max=%d avg=%.3f",
                        tickMs, sample, buckets, m_KpsCur, m_KpsMax,
                        (m_KpsAvgN > 0) ? (m_KpsAvgSum / static_cast<double>(m_KpsAvgN)) : 0.0);
                }
            }
        }

        // Total: sum of the counts of every visible key that is counting.
        if (m_Total.bound) {
            long sum = 0;
            for (int i = 0; i < m_SlotCount; ++i) {
                const Slot &s = m_Slots[i];
                if (s.enabled && s.countEnabled) sum += s.count;
            }
            std::snprintf(m_Total.valueText, sizeof(m_Total.valueText), "%ld", sum);
        }
    }

    // The KPS number is a whole count, exactly like KeyViewer's {CurKPS}, so it
    // is printed as one.
    static void FormatReadout(ReadoutSlot &r) {
        std::snprintf(r.valueText, sizeof(r.valueText), "%d", static_cast<int>(std::lround(r.cachedValue)));
    }

    // -----------------------------------------------------------------------
    // Count persistence
    //
    // Press counts are runtime state, so they are kept in our own JSON file under
    // <game>\ModLoader\Configs instead of BML's .cfg. Two reasons: BML's config
    // only has 5 scalar types (a per-key count would need a visible placeholder
    // property), and writing our own file keeps counts and settings independent,
    // so clearing or exporting counts never touches the user's configuration.
    // -----------------------------------------------------------------------
    static const char *CountFilePath() {
        static char path[MAX_PATH] = {};
        if (path[0] != '\0') return path;

        // Resolve from the executable, NOT the current directory: the game's CWD
        // is <game>\Bin, so a CWD-relative path lands in Bin\ModLoader\... which
        // does not exist. exeDir is <game>\Bin, so go up one level.
        char gameRoot[MAX_PATH] = {};
        if (LocalExecutablePath(gameRoot, sizeof(gameRoot))) {
            // strip the file name
            char *slash = std::strrchr(gameRoot, '\\');
            if (slash != nullptr) *slash = '\0';

            // strip the trailing "\Bin"
            slash = std::strrchr(gameRoot, '\\');
            if (slash != nullptr) *slash = '\0';
        }

        if (gameRoot[0] == '\0') {
            // Fallback: keep the old behaviour rather than writing nowhere.
            std::snprintf(gameRoot, sizeof(gameRoot), ".");
        }

        std::snprintf(path, sizeof(path), "%s\\ModLoader\\Config\\RainingKeys.json", gameRoot);
        return path;
    }

    void SaveCounts() {
        // Counts are always persisted: there is deliberately no config switch for
        // this (the old Main/EnableCountSave option was removed, so the file is
        // written on every exit and restored on the next start).
        char json[4096];
        int written = std::snprintf(json, sizeof(json), "{\n  \"counts\": [");
        for (int i = 0; i < m_BoundSlots && written > 0 && written < static_cast<int>(sizeof(json)) - 32; ++i) {
            written += std::snprintf(json + written, sizeof(json) - written, "%s%ld",
                                     (i == 0) ? "" : ", ", m_Slots[i].count);
        }
        std::snprintf(json + written, sizeof(json) - written, "]\n}\n");

        // Success is confirmed by checking that the file actually exists
        // afterwards, rather than by the return value of the write call.
        LocalWriteTextFile(CountFilePath(), json);
        const bool ok = LocalFileExists(CountFilePath());
        Log("[RainingKeys] counts saved=%d -> %s", ok ? 1 : 0, CountFilePath());
    }

    void ReadCounts() {
        std::string text;
        if (!LocalReadTextFile(CountFilePath(), text)) {
            Log("[RainingKeys] no saved counts yet");
            return;
        }
        const char *p = text.c_str();
        // Minimal parse: collect every integer that appears inside the "counts"
        // array. The file is written by us, so the shape is known.
        p = std::strchr(p, '[');
        if (p != nullptr) {
            ++p;
            int index = 0;
            while (index < m_BoundSlots) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') ++p;
                if (*p == ']' || *p == '\0') break;
                char *end = nullptr;
                const long v = std::strtol(p, &end, 10);
                if (end == p) break;
                m_Slots[index].count = v;
                ++index;
                p = end;
            }
            Log("[RainingKeys] restored counts for %d slots", index);
        }
    }

    void ClearAllCounts() {
        for (int i = 0; i < SLOT_CAPACITY; ++i) m_Slots[i].count = 0;
        SaveCounts();
        // Plain text on purpose: the legacy message board writes straight into
        // a CKSpriteText and would render the ANSI escape codes literally.
        if (m_bml) m_bml->SendIngameMessage("[RainingKeys] all counts cleared.");
        Log("[RainingKeys] all counts cleared");
    }

    // -----------------------------------------------------------------------
    // Commands (typed in the in-game command bar, opened with '/')
    // -----------------------------------------------------------------------
    class ClearCountsCommand final : public ICommand {
    public:
        explicit ClearCountsCommand(RainingKeysMod *owner) : m_Owner(owner) {}

        // The command bar splits the typed line on spaces and looks up args[0],
        // so a name containing a space can never be matched: BML+ 0.3.13 rejects
        // it at registration ("command names ... must be valid UTF-8 tokens
        // without spaces"). Keep it a single token; "/rk clearcounts" is not
        // expressible through this interface.
        std::string GetName() override { return "rkc"; }
        std::string GetAlias() override { return {}; }
        std::string GetDescription() override { return "RainingKeys: reset every press count to zero"; }
        bool IsCheat() override { return false; }

        void Execute(IBML *bml, const std::vector<std::string> & /*args*/) override {
            (void)bml;
            if (m_Owner) m_Owner->ClearAllCounts();
        }

        const std::vector<std::string> GetTabCompletion(IBML *, const std::vector<std::string> &) override {
            return {};
        }

    private:
        RainingKeysMod *m_Owner;
    };

    void RegisterCommands() {
        if (m_bml == nullptr) return;
        m_bml->RegisterCommand(new ClearCountsCommand(this));
    }

    // =======================================================================
    // Drawing entry point. Called from OnProcess only, i.e. before BML calls
    // ImGui::Render(); geometry appended to a draw list after that call is
    // silently discarded (established by the P0 probe build).
    // =======================================================================
    // =======================================================================
    // Coordinate contract
    // -----------------------------------------------------------------------
    // The config carries two independent spaces, and keeping them apart is what
    // makes the display scale as one piece:
    //
    //   LAYOUT space - relative, origin at the BOTTOM-LEFT corner of Key01's
    //     box, X growing right, Y growing UP, unit = one pixel at Scale 100.
    //     Every Key/X, Key/Y, Key/W, Key/H, RainOffsetX/Y, RainLength, RainWidth,
    //     RainSpeed and every KPS/Total X,Y,W,H lives here.
    //
    //   SCREEN space - ImGui viewport pixels, origin at the top-left of the
    //     viewport, Y growing DOWN. Main/OffsetX, OffsetY live here.
    //
    //   screen_x = OffsetX + layout_x * Scale
    //   screen_y = OffsetY - layout_y * Scale
    //
    // Two consequences, both intended:
    //   * Main/OffsetX/Y is the screen position of Key01's bottom-left corner.
    //     It is also the fixed point of Main/Scale: scaling never moves it, so
    //     the display grows and shrinks away from that corner.
    //   * Every LAYOUT distance - including the gaps between the boxes, the
    //     band length and the soft edge - is multiplied by Scale, so shrinking
    //     the display shrinks the gaps with it and the layout keeps its shape.
    //
    // A readout (KPS/Total) is placed exactly like a key box: X,Y is the offset
    // of its CENTRE from that same corner.
    // =======================================================================
    void DrawOverlay() {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        if (vp == nullptr) return;
        const ImVec2 origin = vp->Pos;
        const ImVec2 vpSize = vp->Size;
        if (vpSize.x <= 0.0f || vpSize.y <= 0.0f) return;

        ImDrawList *dl = ImGui::GetForegroundDrawList();
        if (dl == nullptr) return;

        // Both are SCREEN pixels: the corner of the layout and its displacement.
        const float offX = m_CfgOffsetX.GetFloat();
        // Main/OffsetY shares the sign convention of the layout space (positive
        // = up), while the draw code works in screen coordinates where Y grows
        // downwards, so the sign is flipped exactly once, here.
        const float offY = -m_CfgOffsetY.GetFloat();
        const float gScale = Clampf(m_CfgScale.GetFloat(), 5.0f, 1000.0f) * 0.01f;

        // Pass 1: all bands, oldest press first (later presses paint on top)
        DrawAllBands(dl, origin, offX, offY, gScale);

        // Pass 2: the KPS / Total readouts
        DrawReadout(dl, m_Kps, origin, offX, offY, gScale);
        DrawReadout(dl, m_Total, origin, offX, offY, gScale);

        // Pass 3: all key boxes, in slot order
        for (int i = 0; i < m_SlotCount; ++i) {
            if (!m_Slots[i].enabled) continue;
            DrawSlotBox(dl, m_Slots[i], origin, offX, offY, gScale);
        }

        ImGui::Compat::EndFrame();
    }

    // A readout is a plain box: no key binding and no band. It uses the same
    // two-line layout as a key box - a centred label plus the value pinned at the
    // bottom inside - so it matches the key boxes visually.
    void DrawReadout(ImDrawList *dl, const ReadoutSlot &r, const ImVec2 &origin,
                     float offX, float offY, float gScale) {
        if (!r.bound || !r.visible) return;

        const float w = r.w * gScale;
        const float h = r.h * gScale;
        if (w <= 0.5f || h <= 0.5f) return;

        // Same layout space as the key boxes: the centre is Offset + layout*Scale.
        const float cx = origin.x + offX + r.cx * gScale;
        const float cy = origin.y + offY - r.cy * gScale;

        const ImVec2 pmin(cx - w * 0.5f, cy - h * 0.5f);
        const ImVec2 pmax(cx + w * 0.5f, cy + h * 0.5f);

        const float shorter = (w < h) ? w : h;
        const float rounding = Clampf(shorter * ROUND_RATIO, 0.0f, 0.5f * shorter);
        dl->AddRectFilled(pmin, pmax, ToU32(r.bg), rounding);

        // Border, matching the key boxes.
        {
            Color4 oc = r.bg;
            oc.a = 255;
            dl->AddRect(pmin, pmax, ToU32(oc), rounding, 0, OUTLINE_WIDTH * gScale);
        }

        ImFont *font = ImGui::GetFont();

        // Label: centred, shifted up a little to leave room for the value.
        if (r.label[0] != '\0') {
            const float px = BASE_LABEL_SIZE * r.labelSize * gScale;
            const ImVec2 ts = font->CalcTextSizeA(px, 3.4e38f, 0.0f, r.label);
            const float yShift = -h * COUNT_BOTTOM_FRACTION * 0.5f;
            dl->AddText(font, px, ImVec2(cx - ts.x * 0.5f, cy + yShift - ts.y * 0.5f),
                        ToU32(r.labelColor), r.label);
        }

        // Value: fixed position at the bottom inside the box, centred - the same
        // placement the key boxes use for their count.
        //
        // LabelSize and ValueSize are independent: LabelSize scales the label
        // above and nothing else, ValueSize scales this value and nothing else.
        // LabelSize used to be folded in here, which made the readout number grow
        // with the label - something neither setting claims to do. Both formulas
        // agree at the default LabelSize of 1.0, so the default look is unchanged.
        if (r.valueText[0] != '\0') {
            const float px = BASE_LABEL_SIZE * COUNT_SIZE_RATIO * r.valueSize * gScale;
            const ImVec2 ts = font->CalcTextSizeA(px, 3.4e38f, 0.0f, r.valueText);
            const float ty = cy + h * 0.5f - px * (1.0f + COUNT_BOTTOM_FRACTION * 0.5f);
            dl->AddText(font, px, ImVec2(cx - ts.x * 0.5f, ty), ToU32(r.valueColor), r.valueText);
        }
    }

    // =======================================================================
    // Config binding
    // =======================================================================
    // Every Bind* helper reads the property back after setting it, so the cached
    // default always matches what the config actually holds. Without that, a
    // property that did not exist yet would be reported at its in-memory default
    // instead of the value that was just written (a float bound with a non-zero
    // default used to read back as 0).
    Prop BindBool(const char *category, const char *key, const char *comment, bool defVal) {
        Prop out;
        out.b = defVal;
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return out;
        IProperty *p = cfg->GetProperty(category, key);
        if (p == nullptr) return out;
        p->SetComment(comment);
        if (p->GetType() != IProperty::BOOLEAN) {
            p->SetDefaultBoolean(defVal);
            p->SetBoolean(defVal);
        }
        out.p = p;
        out.b = p->GetBoolean() != 0;
        return out;
    }

    Prop BindInt(const char *category, const char *key, const char *comment, int defVal) {
        Prop out;
        out.i = defVal;
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return out;
        IProperty *p = cfg->GetProperty(category, key);
        if (p == nullptr) return out;
        p->SetComment(comment);
        if (p->GetType() != IProperty::INTEGER) {
            p->SetDefaultInteger(defVal);
            p->SetInteger(defVal);
        }
        out.p = p;
        out.i = p->GetInteger();
        return out;
    }

    Prop BindFloat(const char *category, const char *key, const char *comment, float defVal) {
        Prop out;
        out.f = defVal;
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return out;
        IProperty *p = cfg->GetProperty(category, key);
        if (p == nullptr) return out;
        p->SetComment(comment);
        if (p->GetType() != IProperty::FLOAT) {
            p->SetDefaultFloat(defVal);
            p->SetFloat(defVal);
        }
        out.p = p;
        out.f = p->GetFloat();
        return out;
    }

    Prop BindStr(const char *category, const char *key, const char *comment,
                 const char *defVal, int defInt) {
        Prop out;
        out.i = defInt;
        std::snprintf(out.s, sizeof(out.s), "%s", defVal ? defVal : "");
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return out;
        IProperty *p = cfg->GetProperty(category, key);
        if (p == nullptr) return out;
        p->SetComment(comment);
        if (p->GetType() != IProperty::STRING) {
            p->SetDefaultString(defVal ? defVal : "");
            p->SetString(defVal ? defVal : "");
        }
        out.p = p;
        std::snprintf(out.s, sizeof(out.s), "%s", p->GetString());
        return out;
    }

    Prop BindKey(const char *category, const char *key, const char *comment, int defVal) {
        Prop out;
        out.i = defVal;
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return out;
        IProperty *p = cfg->GetProperty(category, key);
        if (p == nullptr) return out;
        p->SetComment(comment);
        if (p->GetType() != IProperty::KEY) {
            p->SetDefaultKey(static_cast<CKKEYBOARD>(defVal));
            p->SetKey(static_cast<CKKEYBOARD>(defVal));
        }
        out.p = p;
        out.i = static_cast<int>(p->GetKey());
        return out;
    }

    // "Main"
    void BindGlobalConfig() {
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return;

        m_CfgEnabled     = BindBool("Main", "Enabled", "Global switch", true);
        m_CfgOnlyInLevel = BindBool("Main", "OnlyInLevel", "Only enable Rainingkeys in level", false);
        m_CfgOffsetX     = BindFloat("Main", "OffsetX", "Global X of RainingKeys (positive = right)", DEFAULT_LAYOUT_CORNER_X);
        m_CfgOffsetY     = BindFloat("Main", "OffsetY", "Global Y of RainingKeys (positive = up)", DEFAULT_LAYOUT_CORNER_Y);
        m_CfgScale       = BindFloat("Main", "Scale", "Global scale of RainingKeys (100 = original size)", 100.0f);
        m_CfgSlotCount   = BindInt("Main", "KeyNums", "How many key slots to use (1-24)", 4);
        m_CfgShowKps     = BindBool("Main", "EnableKPS", "Show KPS and enable its settings", false);
        m_CfgShowTotal   = BindBool("Main", "EnableTotal", "Show total counts of all showed keys and enable its settings", false);
        m_CfgShowDebug   = BindBool("Main", "EnableDebugLog", "Write diagnostic information to the ModLoader log", false);
        cfg->SetCategoryComment("Main", "Global switches, position and scale");
    }

    // Create config properties for slots [m_BoundSlots, count)
    void EnsureSlotsBound(int count) {
        count = Clampi(count, 0, SLOT_CAPACITY);
        if (count <= m_BoundSlots) return;

        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return;

        // First four slots default to the arrow keys Ballance actually uses
        static const int kDefaultKeys[4] = { CKKEY_LEFT, CKKEY_UP, CKKEY_RIGHT, CKKEY_DOWN };

        for (int i = m_BoundSlots; i < count; ++i) {
            Slot &s = m_Slots[i];
            char cat[32];
            std::snprintf(cat, sizeof(cat), "Key%02d", i + 1);

            const int defKey = (i < 4) ? kDefaultKeys[i] : 0;

            // Default placement is relative to the bottom-left corner of Key01's
            // box, so the cross keeps its shape - including its spacing - at any
            // Scale, and a newly added slot lands one default spacing away.
            const float defX = DefaultKeyX(i);
            const float defY = DefaultKeyY(i);

            // Properties are declared in display order; they are shown 4 per page
            // in the menu, so related settings stay together.
            // Property order is the menu page order (4 items per page):
            //   page 1: Key / Label / LabelSize / LabelColor
            //   page 2: X / Y / W / H
            //   page 3: EnableRaining / BgColor / PressedColor / PressedScale
            //   page 4: RainDir / RainOffsetX / RainOffsetY / RainSpeed
            //   page 5: RainLength / RainWidth / RainColor / EnableCount
            //   page 6: CountSize / CountColor
            s.cfgKey          = BindKey  (cat, "Key",           "Bound key. Click the button and press a key", defKey);
            s.cfgLabel        = BindStr  (cat, "Label",         "Text drawn on the box. Empty = use the key name", "", 0);
            s.cfgLabelSize    = BindFloat(cat, "LabelSize",     "Label text size multiplier (1.0 = default size)", 1.0f);
            s.cfgLabelColor   = BindStr  (cat, "LabelColor",    "Label text color as R,G,B,A", "255,255,255,255", 0);
            s.cfgEnableRaining = BindBool(cat, "EnableRaining", "Enable raining for this key", true);
            s.cfgX            = BindFloat(cat, "X",             "Local X of this key", defX);
            s.cfgY            = BindFloat(cat, "Y",             "Local Y of this key", defY);
            s.cfgW            = BindFloat(cat, "W",             "Box width of this key", 90.0f);
            s.cfgH            = BindFloat(cat, "H",             "Box height of this key", 90.0f);
            s.cfgBgColor      = BindStr  (cat, "BgColor",       "Normal box color as R,G,B,A", "26,26,26,180", 0);
            s.cfgPrColor      = BindStr  (cat, "PressedColor",  "Pressed box color as R,G,B,A", "255,255,255,230", 0);
            s.cfgPrScale      = BindFloat(cat, "PressedScale",  "Size multiplier while pressed", 0.9f);
            s.cfgRainDir      = BindStr  (cat, "RainDir",       "Direction of the raining band: Up / Down / Left / Right", "Up", DIR_UP);
            s.cfgRainOffX     = BindFloat(cat, "RainOffsetX",   "Local X of the raining band", 0.0f);
            // Slot order is Left / Up / Right / Down (see kDefaultKeys above), so
            // only the up key flows outwards into empty space: it keeps the small
            // default, while the other three push their origin clear of the cross.
            s.cfgRainOffY     = BindFloat(cat, "RainOffsetY",   "Local Y of the raining band",
                                          (i == 1) ? DEFAULT_RAIN_OFFSET_Y_UP : DEFAULT_RAIN_OFFSET_Y);
            s.cfgRainSpeed    = BindFloat(cat, "RainSpeed",     "Speed of the raining band", 300.0f);
            s.cfgRainLength   = BindFloat(cat, "RainLength",    "Maximum length of the raining band", 300.0f);
            // Slot 4 is the Down key: its band runs back up through the cross, so
            // it defaults to a narrower band than the others. 0 still means "match
            // the box size" for every other slot.
            s.cfgRainWidth    = BindFloat(cat, "RainWidth",     "Width of the raining band (0 = box size)",
                                          (i == 3) ? 80.0f : 0.0f);
            // Key04 is the Down arrow, which keeps its blue band.
            s.cfgRainColor    = BindStr  (cat, "RainColor",     "Band color as R,G,B,A",
                                          (i == 3) ? DEFAULT_RAIN_COLOR_DOWN : DEFAULT_RAIN_COLOR, 0);
            s.cfgEnableCount  = BindBool (cat, "EnableCount",   "Show how many times this key is pressed", true);
            s.cfgCountSize    = BindFloat(cat, "CountSize",     "Count text size multiplier (1.0 = default)", 1.0f);
            s.cfgCountColor   = BindStr  (cat, "CountColor",    "Count text color as R,G,B,A", "255,255,255,255", 0);

            s.bound = true;
        }

        m_BoundSlots = count;
    }

    // Default placement for a fresh config, in LAYOUT units relative to the
    // bottom-left corner of Key01's box. The four arrow keys form a D-pad cross;
    // slots beyond that continue in a row to the right, one spacing apart, so a
    // key added later lands exactly as far from its neighbour as the existing
    // ones - at any Scale, because the spacing is a layout distance.
    float DefaultKeyX(int index) const {
        if (index < DEFAULT_KEY_COUNT) return DEFAULT_KEY_LX[index];
        return DEFAULT_KEY_LX[DEFAULT_KEY_COUNT - 1]
               + DEFAULT_KEY_SPACING * static_cast<float>(index - (DEFAULT_KEY_COUNT - 1));
    }

    float DefaultKeyY(int index) const {
        if (index < DEFAULT_KEY_COUNT) return DEFAULT_KEY_LY[index];
        return DEFAULT_KEY_LY[0];
    }

    // Default centre X of the KPS readout: directly below Key01, i.e. on the
    // corner's vertical line. Total sits one width + one edge gap further right.
    static float DefaultReadoutCornerX() { return DEFAULT_KEY_LX[0]; }

    // One row below the bottom key row, i.e. a NEGATIVE layout Y (the layout
    // space grows upwards; see the coordinate contract).
    static float DefaultReadoutCornerY() { return -DEFAULT_READOUT_DY; }

    // Create the KPS / Total readout properties when their switch is on.
    // These are display-only numbers: no key binding and no rain band, so only
    // the box, text and (for KPS) the sampling parameters exist.
    void EnsureReadoutsBound() {
        IConfig *cfg = GetConfig();
        if (cfg == nullptr) return;

        if (m_CfgShowKps.GetBool() && !m_Kps.bound) {
            ReadoutSlot &r = m_Kps;
            // Same layout space as the key boxes: below Key01 (negative layout Y
            // is downwards) and on its vertical line, one row further down.
            const float kpsX = DefaultReadoutCornerX();
            const float kpsY = DefaultReadoutCornerY();

            r.cfgX          = BindFloat("KPS", "X",          "Local X of this key", kpsX);
            r.cfgY          = BindFloat("KPS", "Y",          "Local Y of this key", kpsY);
            r.cfgW          = BindFloat("KPS", "W",          "Box width of this key", DEFAULT_READOUT_W);
            r.cfgH          = BindFloat("KPS", "H",          "Box height of this key", DEFAULT_READOUT_H);
            r.cfgBgColor    = BindStr  ("KPS", "BgColor",    "Box color as R,G,B,A", "18,18,18,200", 0);
            r.cfgText       = BindStr  ("KPS", "Label",      "Text drawn on the box. Empty = KPS", "", 0);
            r.cfgLabelSize  = BindFloat("KPS", "LabelSize",  "Label text size multiplier (1.0 = default size)", 1.0f);
            r.cfgLabelColor = BindStr  ("KPS", "LabelColor", "Label text color as R,G,B,A", "255,255,255,255", 0);
            r.cfgCountSize  = BindFloat("KPS", "ValueSize",  "Value text size multiplier (1.0 = default)", 1.0f);
            r.cfgCountColor = BindStr  ("KPS", "ValueColor", "Value color as R,G,B,A", "255,255,255,255", 0);
            r.cfgRefresh    = BindFloat("KPS", "KpsRefresh", "KPS refreshing interval in milliseconds", 1000.0f);
            cfg->SetCategoryComment("KPS", "Settings of KPS");
            r.bound = true;
        }

        if (m_CfgShowTotal.GetBool() && !m_Total.bound) {
            ReadoutSlot &r = m_Total;
            // Same row as KPS, placed to its right with the same edge-to-edge gap
            // that separates two adjacent key boxes. Since both boxes are the same
            // width, the centre offset is simply width + gap.
            const float totalX = DefaultReadoutCornerX() + DEFAULT_READOUT_W + DEFAULT_LAYOUT_EDGE_GAP;
            const float totalY = DefaultReadoutCornerY();

            r.cfgX          = BindFloat("Total", "X",         "Local X of this key", totalX);
            r.cfgY          = BindFloat("Total", "Y",         "Local Y of this key", totalY);
            r.cfgW          = BindFloat("Total", "W",         "Box width of this key", DEFAULT_READOUT_W);
            r.cfgH          = BindFloat("Total", "H",         "Box height of this key", DEFAULT_READOUT_H);
            r.cfgBgColor    = BindStr  ("Total", "BgColor",   "Box color as R,G,B,A", "18,18,18,200", 0);
            r.cfgText       = BindStr  ("Total", "Label",     "Text drawn on the box. Empty = Total", "", 0);
            r.cfgLabelSize  = BindFloat("Total", "LabelSize", "Label text size multiplier (1.0 = default size)", 1.0f);
            r.cfgLabelColor = BindStr  ("Total", "LabelColor","Label text color as R,G,B,A", "255,255,255,255", 0);
            r.cfgCountSize  = BindFloat("Total", "ValueSize", "Value text size multiplier (1.0 = default)", 1.0f);
            r.cfgCountColor = BindStr  ("Total", "ValueColor","Value color as R,G,B,A", "255,255,255,255", 0);
            cfg->SetCategoryComment("Total", "Settings of Total");
            // Total is a whole number and always carries its own prefix, so it is
            // formatted directly instead of going through FormatReadout.
            r.bound = true;
        }
    }
    // Read one readout slot's config into its runtime state.
    //
    // defaultLabel is used both for the property's default and when the config
    // holds an empty string, so an empty Label keeps showing "KPS" / "Total" but
    // any text typed in - including the literal word Auto or a lone space - is
    // drawn verbatim, exactly like a key box's Label.
    void ReadReadout(ReadoutSlot &r, bool withSampling, const char *defaultLabel) {
        if (!r.bound) { r.visible = false; return; }

        r.cx = r.cfgX.GetFloat();
        r.cy = r.cfgY.GetFloat();
        r.w = Clampf(r.cfgW.GetFloat(), 2.0f, 4000.0f);
        r.h = Clampf(r.cfgH.GetFloat(), 2.0f, 4000.0f);
        ParseColor(r.cfgBgColor.GetStr(), 18, 18, 18, 200, r.bg.r, r.bg.g, r.bg.b, r.bg.a);

        const char *labelText = r.cfgText.GetStr();
        if (labelText == nullptr || labelText[0] == '\0') labelText = defaultLabel;
        std::snprintf(r.label, sizeof(r.label), "%s", labelText ? labelText : "");
        r.labelSize = Clampf(r.cfgLabelSize.GetFloat(), 0.1f, 8.0f);
        ParseColor(r.cfgLabelColor.GetStr(), 255, 255, 255, 255,
                   r.labelColor.r, r.labelColor.g, r.labelColor.b, r.labelColor.a);
        r.valueSize = Clampf(r.cfgCountSize.GetFloat(), 0.1f, 8.0f);
        ParseColor(r.cfgCountColor.GetStr(), 255, 255, 255, 255,
                   r.valueColor.r, r.valueColor.g, r.valueColor.b, r.valueColor.a);

        if (withSampling) {
            // The tick period may not exceed the 1 second measurement window.
            r.refresh = Clampf(r.cfgRefresh.GetFloat(), 10.0f, KPS_UPDATE_MS);
        }
        r.visible = true;
    }

    // =======================================================================
    // Config -> runtime state
    // =======================================================================
    static void CopyString(char *dst, size_t size, const char *src) {
        if (src == nullptr) src = "";
        std::snprintf(dst, size, "%s", src);
    }

    // Friendly in-memory key names, used for the "Auto" label and as the search
    // order for parsing a typed key name. Ballance's own key names are not
    // friendly enough (or may be empty), so we keep our own table.
    struct KeyNameEntry {
        const char *name;
        int code;
    };

    static const KeyNameEntry *KeyNameTable(int &count) {
        static const KeyNameEntry kTable[] = {
            { "\xE2\x86\x90",   CKKEY_LEFT },      // left arrow
            { "\xE2\x86\x91",   CKKEY_UP },        // up arrow
            { "\xE2\x86\x92",   CKKEY_RIGHT },     // right arrow
            { "\xE2\x86\x93",   CKKEY_DOWN },      // down arrow
            { "Space",          CKKEY_SPACE },
            { "Enter",          CKKEY_RETURN },
            { "NumpadEnter",    CKKEY_NUMPADENTER },
            { "Tab",            CKKEY_TAB },
            { "Esc",            CKKEY_ESCAPE },
            { "Backspace",      CKKEY_BACK },
            { "Delete",         CKKEY_DELETE },
            { "Insert",         CKKEY_INSERT },
            { "Home",           CKKEY_HOME },
            { "End",            CKKEY_END },
            { "PgUp",           CKKEY_PRIOR },
            { "PgDn",           CKKEY_NEXT },
            { "CapsLock",       CKKEY_CAPITAL },
            { "LShift",         CKKEY_LSHIFT },
            { "RShift",         CKKEY_RSHIFT },
            { "LCtrl",          CKKEY_LCONTROL },
            { "RCtrl",          CKKEY_RCONTROL },
            { "LAlt",           CKKEY_LMENU },
            { "RAlt",           CKKEY_RMENU },
            { "LWin",           CKKEY_LWIN },
            { "RWin",           CKKEY_RWIN },
            { "Menu",           CKKEY_APPS },
            { "Minus",          CKKEY_MINUS },
            { "Equals",         CKKEY_EQUALS },
            { "LBracket",       CKKEY_LBRACKET },
            { "RBracket",       CKKEY_RBRACKET },
            { "Semicolon",      CKKEY_SEMICOLON },
            { "Apostrophe",     CKKEY_APOSTROPHE },
            { "Grave",          CKKEY_GRAVE },
            { "Backslash",      CKKEY_BACKSLASH },
            { "Comma",          CKKEY_COMMA },
            { "Period",         CKKEY_PERIOD },
            { "Slash",          CKKEY_SLASH },
            // Bank 0 of the keyboard: 1..9 then 0
            { "1", CKKEY_1 }, { "2", CKKEY_2 }, { "3", CKKEY_3 }, { "4", CKKEY_4 },
            { "5", CKKEY_5 }, { "6", CKKEY_6 }, { "7", CKKEY_7 }, { "8", CKKEY_8 },
            { "9", CKKEY_9 }, { "0", CKKEY_0 },
            // Letters
            { "A", CKKEY_A }, { "B", CKKEY_B }, { "C", CKKEY_C }, { "D", CKKEY_D },
            { "E", CKKEY_E }, { "F", CKKEY_F }, { "G", CKKEY_G }, { "H", CKKEY_H },
            { "I", CKKEY_I }, { "J", CKKEY_J }, { "K", CKKEY_K }, { "L", CKKEY_L },
            { "M", CKKEY_M }, { "N", CKKEY_N }, { "O", CKKEY_O }, { "P", CKKEY_P },
            { "Q", CKKEY_Q }, { "R", CKKEY_R }, { "S", CKKEY_S }, { "T", CKKEY_T },
            { "U", CKKEY_U }, { "V", CKKEY_V }, { "W", CKKEY_W }, { "X", CKKEY_X },
            { "Y", CKKEY_Y }, { "Z", CKKEY_Z },
            // Function keys
            { "F1",  CKKEY_F1 },  { "F2",  CKKEY_F2 },  { "F3",  CKKEY_F3 },
            { "F4",  CKKEY_F4 },  { "F5",  CKKEY_F5 },  { "F6",  CKKEY_F6 },
            { "F7",  CKKEY_F7 },  { "F8",  CKKEY_F8 },  { "F9",  CKKEY_F9 },
            { "F10", CKKEY_F10 }, { "F11", CKKEY_F11 }, { "F12", CKKEY_F12 },
            // Numpad
            { "Num0", CKKEY_NUMPAD0 }, { "Num1", CKKEY_NUMPAD1 }, { "Num2", CKKEY_NUMPAD2 },
            { "Num3", CKKEY_NUMPAD3 }, { "Num4", CKKEY_NUMPAD4 }, { "Num5", CKKEY_NUMPAD5 },
            { "Num6", CKKEY_NUMPAD6 }, { "Num7", CKKEY_NUMPAD7 }, { "Num8", CKKEY_NUMPAD8 },
            { "Num9", CKKEY_NUMPAD9 },
            { "NumAdd",      CKKEY_ADD },
            { "NumSubtract", CKKEY_SUBTRACT },
            { "NumMultiply", CKKEY_MULTIPLY },
            { "NumDivide",   CKKEY_DIVIDE },
            { "NumDecimal",  CKKEY_DECIMAL },
        };
        count = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
        return kTable;
    }

    // Forward lookup: code -> friendly name (returns nullptr when unknown)
    static const char *FriendlyKeyName(int code) {
        int count = 0;
        const KeyNameEntry *table = KeyNameTable(count);
        for (int i = 0; i < count; ++i) {
            if (table[i].code == code) return table[i].name;
        }
        return nullptr;
    }

    // Reverse lookup: typed name -> key code (case insensitive, accepts "VK_xxx"
    // and a few common aliases). Returns 0 when the name is not recognized.
    static int KeyCodeFromString(const char *text) {
        if (text == nullptr || text[0] == '\0') return 0;

        // Pure number: treat it as the raw scan code
        bool numeric = true;
        for (const char *p = text; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') { numeric = false; break; }
        }
        if (numeric) return std::atoi(text);

        const char *name = text;
        if (_strnicmp(name, "VK_", 3) == 0) name += 3;

        int count = 0;
        const KeyNameEntry *table = KeyNameTable(count);
        for (int i = 0; i < count; ++i) {
            if (_stricmp(table[i].name, name) == 0) return table[i].code;
        }
        // A few aliases
        if (_stricmp(name, "Escape") == 0) return CKKEY_ESCAPE;
        if (_stricmp(name, "Return") == 0) return CKKEY_RETURN;
        if (_stricmp(name, "Shift") == 0)  return CKKEY_LSHIFT;
        if (_stricmp(name, "Control") == 0 || _stricmp(name, "Ctrl") == 0) return CKKEY_LCONTROL;
        if (_stricmp(name, "Alt") == 0)    return CKKEY_LMENU;
        if (_stricmp(name, "UpArrow") == 0)    return CKKEY_UP;
        if (_stricmp(name, "DownArrow") == 0)  return CKKEY_DOWN;
        if (_stricmp(name, "LeftArrow") == 0)  return CKKEY_LEFT;
        if (_stricmp(name, "RightArrow") == 0) return CKKEY_RIGHT;
        return 0;
    }

    void ReadConfigAll() {
        m_SlotCount = Clampi(m_CfgSlotCount.GetInt(), 1, m_BoundSlots);
        if (m_SlotCount < 0) m_SlotCount = 0;

        for (int i = 0; i < m_SlotCount; ++i) {
            Slot &s = m_Slots[i];
            if (!s.bound) continue;

            s.keyCode = static_cast<int>(s.cfgKey.GetKey());
            s.enabled = (s.keyCode > 0);

            CopyString(s.labelText, sizeof(s.labelText), s.cfgLabel.GetStr());

            // An EMPTY label means "use the key name"; anything else - including a
            // single space, and including the literal text "Auto" - is drawn
            // exactly as typed, so no value is unreachable any more.
            if (s.labelText[0] == '\0') {
                const char *friendly = FriendlyKeyName(s.keyCode);
                if (friendly != nullptr) {
                    CopyString(s.labelText, sizeof(s.labelText), friendly);
                } else {
                    std::snprintf(s.labelText, sizeof(s.labelText), "Key%d", i + 1);
                }
            }
            s.labelSize = Clampf(s.cfgLabelSize.GetFloat(), 0.1f, 8.0f);
            ParseColor(s.cfgLabelColor.GetStr(), 255, 255, 255, 255,
                       s.labelColor.r, s.labelColor.g, s.labelColor.b, s.labelColor.a);

            s.cx = s.cfgX.GetFloat();
            s.cy = s.cfgY.GetFloat();
            s.w = Clampf(s.cfgW.GetFloat(), 2.0f, 4000.0f);
            s.h = Clampf(s.cfgH.GetFloat(), 2.0f, 4000.0f);

            // Colors carry their own alpha as the 4th component.
            ParseColor(s.cfgBgColor.GetStr(), 26, 26, 26, 180,
                       s.bgNormal.r, s.bgNormal.g, s.bgNormal.b, s.bgNormal.a);
            ParseColor(s.cfgPrColor.GetStr(), 255, 255, 255, 230,
                       s.bgPressed.r, s.bgPressed.g, s.bgPressed.b, s.bgPressed.a);
            ParseColor(s.cfgRainColor.GetStr(), 255, 255, 255, 200,
                       s.rainColor.r, s.rainColor.g, s.rainColor.b, s.rainColor.a);

            s.pressScale = Clampf(s.cfgPrScale.GetFloat(), 0.05f, 4.0f);
            // Easing is fixed to OutQuad (exposed settings were dropped as noise).
            s.pressCurve = EASE_OUT_QUAD;

            s.rainOn = s.cfgEnableRaining.GetBool();
            s.rainDir = DirFromName(s.cfgRainDir.GetStr());
            s.rainOffX = s.cfgRainOffX.GetFloat();
            s.rainOffY = s.cfgRainOffY.GetFloat();
            s.rainSpeed = Clampf(s.cfgRainSpeed.GetFloat(), 0.0f, 10000.0f);
            s.rainWidth = Clampf(s.cfgRainWidth.GetFloat(), 0.0f, 2000.0f);

            // RainLength stays in configured units; the band's head/tail live in
            // the same space and the global Scale is applied when the position is
            // converted to pixels.
            s.rainLen = Clampf(s.cfgRainLength.GetFloat(), 0.0f, 8000.0f);

            // Count readout
            const bool countWasEnabled = s.countEnabled;
            s.countEnabled = s.cfgEnableCount.GetBool();
            s.countSize = Clampf(s.cfgCountSize.GetFloat(), 0.1f, 8.0f);
            ParseColor(s.cfgCountColor.GetStr(), 255, 255, 255, 255,
                       s.countColor.r, s.countColor.g, s.countColor.b, s.countColor.a);
            (void)countWasEnabled;
        }
    }

    int CountActive() const {
        int n = 0;
        for (int i = 0; i < m_BoundSlots; ++i) {
            if (m_Slots[i].enabled) ++n;
        }
        return n;
    }

    // =======================================================================
    // Animation
    // =======================================================================
    void AdvanceAnimation(Slot &s, float dt) {
        s.tAnim += dt;

        const float k = ApplyEase(s.pressCurve, Progress(s.tAnim, PRESS_DURATION));
        const float toScale = s.pressed ? s.pressScale : 1.0f;
        const Color4 toBg = s.pressed ? s.bgPressed : s.bgNormal;

        s.curScale = s.fromScale + (toScale - s.fromScale) * k;
        s.curBg = LerpColor(s.fromBg, toBg, k);
    }

    // =======================================================================
    // Bands
    // =======================================================================
    void SpawnBand(Slot &s, float now) {
        if (!s.rainOn) return;

        // Reuse a free entry, or grow the pool. Growing is preferred over
        // recycling the oldest band so rapid presses never truncate a live band;
        // the pool only stops growing at BAND_POOL_MAX.
        Slot::Band *target = nullptr;
        for (auto &b : s.bands) {
            if (!b.alive) { target = &b; break; }
        }
        if (target == nullptr) {
            if (static_cast<int>(s.bands.size()) < BAND_POOL_MAX) {
                s.bands.emplace_back();
                target = &s.bands.back();
            } else {
                // Hard cap reached: fall back to recycling the oldest band.
                float oldest = 1e30f;
                for (auto &b : s.bands) {
                    if (b.startTime < oldest) { oldest = b.startTime; target = &b; }
                }
            }
        }
        if (target == nullptr) return;

        target->alive = true;
        target->held = true;
        target->drifting = false;
        target->startTime = now;
        target->head = 0.0f;
        target->tail = 0.0f;

        // Freeze the color at birth: the band has its own color and never follows
        // the box, so releasing the key (which fades the box back to its normal
        // color) must not tint the band.
        target->color = s.rainColor;
    }

    // Fade distance, in LAYOUT units: the band is solid up to
    // (limit - fadeDist) and its alpha ramps to zero at the limit with a
    // smoothstep curve, so the soft edge is fadeDist * Scale pixels tall.
    //
    // The distance is the fixed FADE_LENGTH and does not depend on the scale,
    // because the caller already multiplies every layout distance by Scale
    // (this used to divide a 1-screen-pixel floor by the scale, which made the
    // soft edge depend on the scale twice over). The only remaining clamp is
    // the degenerate case: a band shorter than the fade would never reach full
    // alpha anywhere, so the fade is capped at the length limit itself.
    static float FadeDistance(float limit) {
        return (FADE_LENGTH < limit) ? FADE_LENGTH : limit;
    }

    // Release transition. Two cases, depending on whether the head already
    // reached the length limit while the key was held:
    //   head >= limit : the head freezes; only the tail travels up, so the
    //                   rectangle is eaten from below and vanishes at the limit.
    //   head <  limit : both edges travel up together, so the rectangle keeps
    //                   its height and drifts until the head reaches the limit;
    //                   it then switches to the first case automatically.
    void ReleaseBands(Slot &s) {
        for (auto &b : s.bands) {
            if (!b.alive || !b.held) continue;

            b.held = false;
            if (b.head >= s.rainLen) {
                b.head = s.rainLen;
                b.drifting = false;
            } else {
                b.drifting = true;
            }
        }
    }

    // head/tail/limit all live in LAYOUT units, so the speed must too: RainSpeed
    // is layout units per second, exactly like RainLength. The band therefore
    // needs RainLength / RainSpeed seconds to reach its limit AT ANY SCALE - the
    // whole travel is scaled at once, so scaling it here as well would double
    // count and make the band crawl at small scales.
    void AdvanceBands(Slot &s, float dt) {
        const float step = s.rainSpeed * dt;
        const float limit = s.rainLen;

        for (auto &b : s.bands) {
            if (!b.alive) continue;

            if (b.held) {
                // Pinned lower edge, growing upper edge. Capped at the limit:
                // everything past it is fully transparent anyway.
                b.head += step;
                if (b.head > limit) b.head = limit;
            } else if (b.drifting) {
                // Released before reaching the limit: both edges travel up
                // together, so the rectangle keeps its height and drifts.
                b.head += step;
                b.tail += step;

                if (b.head >= limit) {
                    b.head = limit;
                    b.drifting = false;
                }
            } else {
                // Released at/after the limit: the upper edge is frozen and the
                // lower edge travels up to meet it, dissolving into the fade.
                b.tail += step;
            }

            // Both edges are monotonic; the band dies when they meet.
            if (b.tail >= b.head) {
                b.alive = false;
                b.held = false;
                b.drifting = false;
                b.head = 0.0f;
                b.tail = 0.0f;
            }
        }
    }

    // =======================================================================
    // Drawing
    // =======================================================================
    struct SlotGeom {
        float x = 0.0f, y = 0.0f;
        float w = 0.0f, h = 0.0f;
    };

    // Maps a slot's LAYOUT coordinates to screen pixels. offX/offY are already
    // screen pixels (the layout corner, see the contract above DrawOverlay) and
    // every layout distance is multiplied by gScale, so the boxes and the gaps
    // between them scale together.
    SlotGeom ComputeGeom(const Slot &s, float offX, float offY, float gScale) const {
        SlotGeom g;
        g.x = offX + s.cx * gScale;
        g.y = offY - s.cy * gScale;   // layout Y grows up, screen Y grows down
        g.w = s.w * gScale;
        g.h = s.h * gScale;
        return g;
    }

    void RainOrigin(const Slot &s, const SlotGeom &g, const ImVec2 &origin,
                    float gScale, float &outX, float &outY) const {
        const float hw = g.w * 0.5f;
        const float hh = g.h * 0.5f;
        const float cx = origin.x + g.x;
        const float cy = origin.y + g.y;
        const float offX = s.rainOffX * gScale;
        const float offY = s.rainOffY * gScale;

        switch (s.rainDir) {
            case DIR_UP:    outX = cx + offX;      outY = cy - hh - offY; break;
            case DIR_DOWN:  outX = cx + offX;      outY = cy + hh + offY; break;
            case DIR_LEFT:  outX = cx - hw - offX; outY = cy + offY;      break;
            case DIR_RIGHT: outX = cx + hw + offX; outY = cy + offY;      break;
            default:        outX = cx;             outY = cy - hh;        break;
        }
    }

    void DrawAllBands(ImDrawList *dl, const ImVec2 &origin, float offX, float offY, float gScale) {
        m_BandOrder.clear();
        for (int i = 0; i < m_SlotCount; ++i) {
            const Slot &s = m_Slots[i];
            if (!s.enabled || !s.rainOn) continue;
            for (int b = 0; b < static_cast<int>(s.bands.size()); ++b) {
                if (s.bands[b].alive) m_BandOrder.emplace_back(s.bands[b].startTime, i, b);
            }
        }
        if (m_BandOrder.empty()) return;

        std::sort(m_BandOrder.begin(), m_BandOrder.end(),
                  [](const BandRef &a, const BandRef &b) {
                      if (a.t != b.t) return a.t < b.t;
                      if (a.slot != b.slot) return a.slot < b.slot;
                      return a.band < b.band;
                  });

        for (const BandRef &ref : m_BandOrder) {
            const Slot &s = m_Slots[ref.slot];
            const SlotGeom g = ComputeGeom(s, offX, offY, gScale);
            DrawOneBand(dl, s, g, origin, s.bands[ref.band], gScale);
        }
    }

    void DrawOneBand(ImDrawList *dl, const Slot &s, const SlotGeom &g,
                     const ImVec2 &origin, const Slot::Band &band, float gScale) {
        // The band is solid up to (limit - fadeDist) and its alpha ramps to zero
        // at the limit, following a smoothstep curve - the same shape Unity uses
        // for RectMask2D softness, which is the mechanism KeyViewer relies on.
        // The fade is measured back from the limit, so the soft edge always sits
        // at the limit no matter which band edge is approaching it.
        const float limit = s.rainLen;
        const float fadeDist = FadeDistance(limit);
        const float fadeLo = limit - fadeDist;   // alpha still full here

        const float lo = (band.tail < 0.0f) ? 0.0f : band.tail;   // clip below the key edge
        const float hi = (band.head > limit) ? limit : band.head; // clip at the limit
        if (hi - lo <= 0.5f / gScale) return;   // thinner than half a screen pixel

        float px = 0.0f, py = 0.0f;
        RainOrigin(s, g, origin, gScale, px, py);

        const float width = (s.rainWidth > 0.0f)
                                ? (s.rainWidth * gScale)
                                : (IsVertical(s.rainDir) ? g.w : g.h);

        const int baseAlpha = band.color.a;

        // alpha factor (0..1) for a distance from the key edge.
        // Above fadeLo it is 1 - smoothstep, so it eases out instead of ramping
        // linearly (which is what made the previous version look abrupt).
        auto factorAt = [&](float d) -> float {
            if (fadeDist <= 0.0f) return (d <= limit) ? 1.0f : 0.0f;
            float t = (d - fadeLo) / fadeDist;
            if (t <= 0.0f) return 1.0f;
            if (t >= 1.0f) return 0.0f;
            return 1.0f - (t * t * (3.0f - 2.0f * t));   // 1 - smoothstep
        };

        auto colorAt = [&](float d) -> ImU32 {
            Color4 c = band.color;   // frozen at spawn; never follows the box
            c.a = static_cast<int>(std::lround(baseAlpha * factorAt(d)));
            return ToU32(c);
        };

        // Maps a distance-from-key-edge range to screen corners. The `colorLo`
        // and `colorHi` arguments are the colors for the *near* and *far* ends
        // along the flow direction, so the caller can pass a gradient.
        //
        // fromD/toD arrive in LAYOUT units and must be multiplied by gScale here,
        // exactly like the box geometry and the band width: the distance along
        // the flow is what carries the band's LENGTH and the position of the soft
        // edge, so forgetting the scale here pins both to a fixed pixel size.
        auto quad = [&](float fromD, float toD, ImU32 colorNear, ImU32 colorFar) {
            if (toD - fromD <= 0.01f) return;

            const float from = fromD * gScale;
            const float to = toD * gScale;

            float x0, y0, x1, y1;
            switch (s.rainDir) {
                case DIR_UP:
                    x0 = px - width * 0.5f; x1 = px + width * 0.5f;
                    y0 = py - to;           y1 = py - from;
                    break;
                case DIR_DOWN:
                    x0 = px - width * 0.5f; x1 = px + width * 0.5f;
                    y0 = py + from;         y1 = py + to;
                    break;
                case DIR_LEFT:
                    y0 = py - width * 0.5f; y1 = py + width * 0.5f;
                    x0 = px - to;           x1 = px - from;
                    break;
                case DIR_RIGHT:
                default:
                    y0 = py - width * 0.5f; y1 = py + width * 0.5f;
                    x0 = px + from;         x1 = px + to;
                    break;
            }
            if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;

            // Map the "far along the flow" end onto the rectangle's corners.
            // Up and Left travel towards smaller X/Y, so there the far end is the
            // top / left edge; Down and Right are the other way round.
            ImU32 tl, tr, br, bl;
            if (s.rainDir == DIR_UP) {
                tl = tr = colorFar;
                bl = br = colorNear;
            } else if (s.rainDir == DIR_LEFT) {
                tl = bl = colorFar;
                tr = br = colorNear;
            } else if (s.rainDir == DIR_DOWN) {
                tl = tr = colorNear;
                bl = br = colorFar;
            } else {   // DIR_RIGHT
                tl = bl = colorNear;
                tr = br = colorFar;
            }

            dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), tl, tr, br, bl);
        };

        // 1) Solid part: from lo up to wherever the fade zone begins (or hi).
        const float solidEnd = (hi < fadeLo) ? hi : fadeLo;
        if (solidEnd > lo) {
            quad(lo, solidEnd, colorAt(lo), colorAt(solidEnd));
        }

        // 2) Fade part: [max(lo, fadeLo), hi]. The alpha at the start is taken
        //    from the actual position, so when the tail rises into the fade zone
        //    the dissolve stays continuous instead of snapping to full alpha.
        const float fadeStart = (lo > fadeLo) ? lo : fadeLo;
        if (hi > fadeStart) {
            quad(fadeStart, hi, colorAt(fadeStart), colorAt(hi));
        }
    }

    void DrawSlotBox(ImDrawList *dl, const Slot &s, const ImVec2 &origin,
                     float offX, float offY, float gScale) {
        const SlotGeom g = ComputeGeom(s, offX, offY, gScale);

        const float sw = g.w * s.curScale;
        const float sh = g.h * s.curScale;
        const ImVec2 pmin(origin.x + g.x - sw * 0.5f, origin.y + g.y - sh * 0.5f);
        const ImVec2 pmax(origin.x + g.x + sw * 0.5f, origin.y + g.y + sh * 0.5f);

        // Roundness follows the box's shorter side (ratio, not pixels), so it
        // stays visually correct at other box sizes / resolutions.
        const float shorter = (sw < sh) ? sw : sh;
        const float rounding = Clampf(shorter * ROUND_RATIO, 0.0f, 0.5f * shorter);
        dl->AddRectFilled(pmin, pmax, ToU32(s.curBg), rounding);

        // Outline is always on; thickness scales with the display.
        {
            Color4 oc = s.curBg;
            oc.a = 255;
            dl->AddRect(pmin, pmax, ToU32(oc), rounding, 0, OUTLINE_WIDTH * gScale);
        }

        // Label: always white, size = base * per-key multiplier * scale.
        ImFont *font = ImGui::GetFont();
        const float labelPx = BASE_LABEL_SIZE * s.labelSize * gScale;
        const bool wantCount = s.countEnabled;

        if (s.labelText[0] != '\0') {
            const char *text = s.labelText;
            const ImVec2 ts = font->CalcTextSizeA(labelPx, 3.4e38f, 0.0f, text);
            // With a count line below, the label sits a little higher so the two
            // lines do not overlap.
            const float yShift = wantCount ? (-g.h * COUNT_BOTTOM_FRACTION * 0.5f) : 0.0f;
            const float tx = origin.x + g.x - ts.x * 0.5f;
            const float ty = origin.y + g.y + yShift - ts.y * 0.5f;

            dl->AddText(font, labelPx, ImVec2(tx, ty), ToU32(s.labelColor), text);
        }

        // Count: fixed position at the bottom inside the box, centered.
        if (wantCount) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%ld", s.count);

            // LabelSize deliberately does NOT appear here: the label's multiplier
            // and the count's are independent settings, so changing one must not
            // resize the other. LabelSize used to be folded into the count size,
            // which meant raising it grew the count as well, while CountSize only
            // ever grew the count. At the default LabelSize of 1.0 the two
            // formulas are identical, so the default look does not change.
            const float countPx = BASE_LABEL_SIZE * COUNT_SIZE_RATIO * s.countSize * gScale;
            const ImVec2 ts = font->CalcTextSizeA(countPx, 3.4e38f, 0.0f, buf);

            const float tx = origin.x + g.x - ts.x * 0.5f;
            const float ty = origin.y + g.y + g.h * 0.5f
                             - countPx * (1.0f + COUNT_BOTTOM_FRACTION * 0.5f);

            dl->AddText(font, countPx, ImVec2(tx, ty), ToU32(s.countColor), buf);
        }
    }

    // =======================================================================
    // Logging
    // =======================================================================
    void Log(const char *fmt, ...) {
        ILogger *log = GetLogger();
        if (log == nullptr) return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log->Info(buf);
    }

    // =======================================================================
    // Members
    // =======================================================================
    Prop m_CfgEnabled, m_CfgOnlyInLevel, m_CfgOffsetX, m_CfgOffsetY, m_CfgScale;
    Prop m_CfgSlotCount, m_CfgShowDebug, m_CfgShowKps, m_CfgShowTotal;

    Slot m_Slots[SLOT_CAPACITY];
    int  m_SlotCount = 8;      // slots read from config
    int  m_BoundSlots = 0;     // slots whose properties exist

    ReadoutSlot m_Kps;
    ReadoutSlot m_Total;

    std::vector<BandRef> m_BandOrder;

    unsigned m_FrameCount = 0;
    unsigned m_RenderCount = 0;
    float    m_LastTime = -1.0f;
    float    m_TotalTime = 0.0f;
    ImVec2   m_ViewportSize{1600.0f, 1200.0f};

    // Global KPS state (KeyViewer's model): a ring of buckets, each covering the
    // tick period, summed over the last KPS_UPDATE_MS. Max and Average are
    // maintained the same way KeyViewer does, even though only Kps is displayed.
    int      m_KpsRing[KPS_RING_MAX] = {};   // press count per completed bucket
    int      m_KpsRingHead = 0;              // next write position
    int      m_KpsPressCount = 0;            // presses collected in the open bucket
    float    m_KpsElapsedMs = 0.0f;          // time into the open bucket
    int      m_KpsCur = 0;                   // = KeyViewer's KPSCalculator.Kps
    int      m_KpsMax = 0;                   // = KeyViewer's KPSCalculator.Max
    double   m_KpsAvgSum = 0.0;              // running sum for Average (0 samples skipped)
    long     m_KpsAvgN = 0;
    long     m_LastPressTotal = 0;

    // Diagnostics snapshot (filled in OnProcess, drawn/logged later)
    bool m_ShowEnabled = true;
    bool m_ShowOnlyInLevel = true;
    bool m_IsIngame = false;
    bool m_WantCaptureKeyboard = false;
    bool m_WantTextInput = false;
    int  m_RenderFailReason = 0;
};

} // namespace

// ===========================================================================
MOD_EXPORT IMod *BMLEntry(IBML *bml) {
    return new RainingKeysMod(bml);
}
