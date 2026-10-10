// ===========================================================================
// rk_imgui_compat.cpp - the Virtools 2D backend behind rk_imgui_compat.hpp
// ---------------------------------------------------------------------------
// Everything here exists to make ../bml-old/RainingKeys.cpp look like the BML+
// source while drawing through Virtools 2D entities. Three ideas carry the
// whole thing:
//
//   1. COMMAND BUFFER INSTEAD OF A DRAW LIST.
//      The ImGui facade only ever appends primitives in painting order, so the
//      facade records them and realises them once per frame in EndFrame().
//
//   2. Z-ORDER FROM THE SUBMISSION INDEX.
//      A retained-mode scene has no "last drawn wins"; it sorts by z-order.
//      EndFrame() therefore hands out  z = Z_BASE + command_index, which
//      reproduces painter's order exactly - the three-pass structure of
//      DrawOverlay (bands, then readouts, then boxes) keeps working untouched.
//
//   3. POOLED ELEMENTS, NEVER CREATED PER FRAME.
//      Every quad is a CK2dEntity + CKMaterial pair, exactly like BGui::Panel
//      (which is why this is known to render), and every text element is a
//      BGui::Text, i.e. the same CKSpriteText path BML's own menus use. Both
//      are created lazily, reused, and merely hidden when a frame does not
//      need them.
//
// STEP 2 SCOPE. Rounded boxes and the band fade are no longer built out of
// flat quads: both read a texture rasterised once by the factory below.
//
//   * A rounded fill or outline is one sprite showing a generated mask, so the
//     corners are antialiased and a box costs 2 elements instead of 5.
//   * A two-stop gradient is one sprite cropping a linear alpha ramp, so a band
//     costs 2 elements instead of 13 (the previous constant-alpha strip stack).
//     The crop is taken over the ALPHA FRACTIONS of the two endpoint colours,
//     not over distance, which reproduces ImGui's linear interpolation exactly.
//
// Plain axis-aligned fills keep the textureless path, which is both cheaper and
// the one already proven to render correctly with a translucent diffuse colour.
// Shape alpha is baked into the mask textures rather than taken from the
// material's alpha, so nothing depends on how this rasteriser folds the two.
// ===========================================================================

#include "rk_imgui_compat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
// Base z-order. BGui keeps everything it owns in 0..25 (Panel 0, buttons and
// Text 20..25), so 30 puts the overlay above BML's own UI. Whether it should
// also sit above BML's Mod Options menu is a taste question we tune from
// screenshots, not a correctness one.
constexpr int Z_BASE = 30;

// A gradient is no longer a stack of strips: it is one sprite reading a
// generated alpha ramp (see GetRampTexture / GradientRect). Only the shape
// alpha has to be quantised, into this many levels.

// Font size calibration.
//
// BML+ draws through ImGui's default font: ProggyClean, baked at 13px with 9px
// capitals, and AddText(font, N) scales the glyphs by N / 13. A requested size
// N therefore renders capitals of about 9N/13.
//
// CKSpriteText::SetFont(), by contrast, renders capitals of roughly N pixels
// (measured from a screenshot: a count drawn at a nominal 19.5 came out 19px
// tall). Handing it the ImGui size unchanged makes every label about 45% too
// big, so the request is scaled to match what BML+ would have drawn.
constexpr float FONT_EM_TO_CAP = 9.0f / 13.0f;

// Text surfaces are sized to hold the line plus slack. The width only has to
// be generous because the sprite centres the text itself.
constexpr float TEXT_HEIGHT_RATIO = 1.6f;
constexpr float TEXT_WIDTH_PER_CHAR = 0.8f;
constexpr float TEXT_MIN_WIDTH_FACTOR = 2.0f;
constexpr float TEXT_MAX_WIDTH_FACTOR = 24.0f;

// ---------------------------------------------------------------------------
// Step 2: generated textures
// ---------------------------------------------------------------------------
// A shape is rasterised once at a size snapped to this grid and then stretched
// to the exact rectangle. That is safe because everything this mod rounds or
// fades is scaled uniformly: a rounded box's radius is a fixed fraction of its
// shorter side, so scaling the mask scales the radius by the same factor and
// the result stays exact. Snapping matters because the press animation changes
// the box size every frame: at 16px a 90px box shrinking to 90% stays inside a
// single bucket, so one mask serves the whole animation instead of one per
// frame.
constexpr int MASK_QUANT_PX = 16;

// Shape alpha is baked into the texture rather than taken from the material's
// diffuse, so nothing here depends on whether the rasteriser multiplies the
// texture alpha by the diffuse alpha. That is what keeps the whole step safe:
// only RGB modulation (the canonical meaning of MODULATE) is relied upon.
// 16 levels is plenty - the box alpha only ever moves between 180 and 230, so
// the quantisation error stays under 3% of full scale.
constexpr int ALPHA_LEVELS = 16;

// Ramp textures are square and RAMP_TEXELS on a side. The ramp runs along one
// axis and is constant along the other, so any sub-range of the constant axis
// can be sampled. A 1-pixel dimension is deliberately avoided: a 1x256 ramp was
// the only texture shape that crashed the engine (and the only one that never
// produced a dump), and a square canvas also stays safely power-of-two.
constexpr int RAMP_TEXELS = 64;

// Bounded to keep a runaway configuration from allocating forever. Real usage
// is a few dozen entries.
constexpr size_t TEX_CACHE_MAX = 192;

// ===========================================================================
// Host pointers
// ===========================================================================
IBML* g_Bml = nullptr;
ILogger* g_Log = nullptr;

bool g_Inited = false;
bool g_Probed = false;
bool g_LoggedFontOnce = false;

// SetPosition semantics, resolved by probing the engine once. Virtools' 2D
// entities are hot-spot anchored, and the header says nothing about whether the
// hot spot is the centre or the top-left corner. BGui's own hit test
// (Gui::Intersect) treats GetPosition() as the top-left, while its panel
// placement maths (Panel::SetPosition((x0+w/2)/W, ...)) treats it as the
// centre - the two cannot both be right, so we ask the engine instead of
// guessing: 0.5 means "offset the position by half the size to get a top-left".
float g_HotSpotFactor = 0.5f;

// The same question for CKSpriteText, which is a different class from the plain
// CK2dEntity the main probe measures. Probing it separately is not paranoia: the
// first legacy screenshot showed every label displaced right and down by half
// its surface, which is exactly the signature of passing a centre to a
// top-left-anchored setter.
float g_TextHotSpotFactor = 0.5f;

// True once a frame has reached EndFrame(). BeginFrame() only has to hide the
// previous frame's elements when the frame before it never completed, i.e. when
// the mod was gated off (master switch, or OnlyInLevel while in a menu) and
// OnProcess returned before DrawOverlay. In the normal case the elements are
// simply reused, which saves two Show() calls per element per frame.
bool g_FrameCompleted = true;

// Render-context size, refreshed once per frame instead of once per quad.
float g_VpW = 1600.0f;
float g_VpH = 1200.0f;

// ===========================================================================
// Colour helpers
//
// ImU32 (and therefore IM_COL32) is ImGui-packed: A<<24 | B<<16 | G<<8 | R.
// Virtools packs A<<24 | R<<16 | G<<8 | B, which is what RGBAITOCOLOR and
// VxColor(unsigned int) expect.
// ===========================================================================
inline int ChanR(ImU32 c) { return static_cast<int>(c & 0xFFu); }
inline int ChanG(ImU32 c) { return static_cast<int>((c >> 8) & 0xFFu); }
inline int ChanB(ImU32 c) { return static_cast<int>((c >> 16) & 0xFFu); }
inline int ChanA(ImU32 c) { return static_cast<int>((c >> 24) & 0xFFu); }

inline ImU32 PackIm(int r, int g, int b, int a) {
    return (static_cast<ImU32>(a) << 24) | (static_cast<ImU32>(b) << 16) |
           (static_cast<ImU32>(g) << 8) | static_cast<ImU32>(r);
}

inline VxColor VxOf(ImU32 c) {
    // The explicit unsigned cast matters: RGBAITOCOLOR is an int expression and
    // VxColor has both VxColor(unsigned int) and VxColor(float) overloads.
    return VxColor(
        static_cast<unsigned int>(RGBAITOCOLOR(ChanR(c), ChanG(c), ChanB(c), ChanA(c))));
}

// Tint for a textured element: the alpha already lives in the texture, so the
// diffuse is left opaque and the result is the same whether or not the
// rasteriser folds the diffuse alpha into the texture alpha.
inline VxColor VxOfTint(ImU32 c) {
    return VxColor(static_cast<unsigned int>(RGBAITOCOLOR(ChanR(c), ChanG(c), ChanB(c), 255)));
}

inline CKDWORD VxTextColorOf(ImU32 c) {
    return static_cast<CKDWORD>(RGBAITOCOLOR(ChanR(c), ChanG(c), ChanB(c), ChanA(c)));
}

// ===========================================================================
// Command buffer
// ===========================================================================
struct Cmd {
    enum Kind { FILL = 0, OUTLINE, GRADIENT, TEXT } kind = FILL;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    ImU32 c0 = 0, c1 = 0;
    ImU32 c2 = 0, c3 = 0;
    float rounding = 0.0f;
    float thickness = 0.0f;
    float fontSize = 0.0f;
    char text[64] = {};
};

std::vector<Cmd> g_Cmds;
unsigned g_LastCommands = 0;

// ===========================================================================
// Quad elements
// ===========================================================================
struct Quad {
    CK2dEntity* ent = nullptr;
    CKMaterial* mat = nullptr;

    // Last state pushed to the engine; everything is change-cached because a
    // 2D entity's setters walk engine-side state we would rather not churn
    // every frame for every element.
    float x0 = 0.0f, y0 = 0.0f, w = -1.0f, h = -1.0f;
    ImU32 col = 0;
    int z = 0;
    bool visible = false;
    bool hasRect = false;
};

std::vector<Quad> g_Quads;
size_t g_QuadNext = 0;

// Textured elements are kept in their own pool: UseSourceRect is a per-entity
// switch that is set once when the element is created, so an element must never
// migrate between the plain and the textured kind.
struct TexQuad {
    CK2dEntity* ent = nullptr;
    CKMaterial* mat = nullptr;
    CKTexture* tex = nullptr;

    float x0 = 0.0f, y0 = 0.0f, w = -1.0f, h = -1.0f;
    VxRect src;
    bool hasSrc = false;
    ImU32 col = 0;
    int z = 0;
    bool visible = false;
    bool hasRect = false;
};

std::vector<TexQuad> g_TexQuads;
size_t g_TexQuadNext = 0;

// ===========================================================================
// Text elements
// ===========================================================================
struct TextElem {
    BGui::Text* text = nullptr;

    float px = -1.0f;          // font pixel size currently applied to the sprite
    float w = -1.0f, h = -1.0f;  // surface size currently applied, in pixels
    char content[64] = {};     // text currently rasterised into the surface
    CKDWORD col = 0;
    float cx = 0.0f, cy = 0.0f;  // top-left of the surface rect currently applied
    int z = 0;
    bool visible = false;
};

std::vector<TextElem> g_Texts;
size_t g_TextNext = 0;

// ===========================================================================
// Element creation
// ===========================================================================
CKLevel* CurrentLevel() {
    if (g_Bml == nullptr) return nullptr;
    CKContext* ctx = g_Bml->GetCKContext();
    if (ctx == nullptr) return nullptr;
    return ctx->GetCurrentLevel();
}

// Replicates BGui::Panel's construction (a 2D entity carrying an alpha-blended
// material), plus the NotToBeListedAndSaved flag so these transient overlays
// never end up in a saved level.
Quad* CreateQuad() {
    if (g_Bml == nullptr) return nullptr;
    CKContext* ctx = g_Bml->GetCKContext();
    CKLevel* level = CurrentLevel();
    if (ctx == nullptr || level == nullptr) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "RK_Quad_%u", static_cast<unsigned>(g_Quads.size()));

    CK2dEntity* ent = static_cast<CK2dEntity*>(ctx->CreateObject(CKCID_2DENTITY, name));
    if (ent == nullptr) return nullptr;

    ent->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
    level->AddObject(ent);
    ent->SetHomogeneousCoordinates();
    ent->EnableClipToCamera(FALSE);
    ent->EnableRatioOffset(FALSE);
    ent->SetZOrder(Z_BASE);
    ent->Show(CKHIDE);

    char matName[72];
    std::snprintf(matName, sizeof(matName), "%s_Mat", name);
    CKMaterial* mat = static_cast<CKMaterial*>(ctx->CreateObject(CKCID_MATERIAL, matName));
    if (mat != nullptr) {
        mat->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
        level->AddObject(mat);
        mat->EnableAlphaBlend();
        mat->SetSourceBlend(VXBLEND_SRCALPHA);
        mat->SetDestBlend(VXBLEND_INVSRCALPHA);
        ent->SetMaterial(mat);
    }

    Quad q;
    q.ent = ent;
    q.mat = mat;
    g_Quads.push_back(q);
    return &g_Quads.back();
}

Quad* AcquireQuad() {
    if (g_QuadNext < g_Quads.size()) return &g_Quads[g_QuadNext++];
    Quad* q = CreateQuad();
    if (q == nullptr) return nullptr;
    ++g_QuadNext;
    return q;
}

// Replicates BGui::Button's construction instead: a 2D entity that renders a
// sub-rect of a texture. UseSourceRect is set once here, which is why textured
// elements live in their own pool.
TexQuad* CreateTexQuad() {
    if (g_Bml == nullptr) return nullptr;
    CKContext* ctx = g_Bml->GetCKContext();
    CKLevel* level = CurrentLevel();
    if (ctx == nullptr || level == nullptr) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "RK_Tex_%u", static_cast<unsigned>(g_TexQuads.size()));

    CK2dEntity* ent = static_cast<CK2dEntity*>(ctx->CreateObject(CKCID_2DENTITY, name));
    if (ent == nullptr) return nullptr;

    ent->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
    level->AddObject(ent);
    ent->SetHomogeneousCoordinates();
    ent->EnableClipToCamera(FALSE);
    ent->EnableRatioOffset(FALSE);
    ent->SetZOrder(Z_BASE);
    ent->UseSourceRect(TRUE);
    ent->Show(CKHIDE);

    char matName[72];
    std::snprintf(matName, sizeof(matName), "%s_Mat", name);
    CKMaterial* mat = static_cast<CKMaterial*>(ctx->CreateObject(CKCID_MATERIAL, matName));
    if (mat != nullptr) {
        mat->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
        level->AddObject(mat);
        mat->EnableAlphaBlend();
        mat->SetSourceBlend(VXBLEND_SRCALPHA);
        mat->SetDestBlend(VXBLEND_INVSRCALPHA);
        // MODULATEALPHA, not MODULATE. Virtools' plain MODULATE is a colour-only
        // modulation whose alpha comes from the diffuse, which made every
        // textured shape fully opaque; the *ALPHA variants are the ones that
        // fold the texture's alpha channel into the result. The diffuse alpha
        // is left at 255 by VxOfTint, so under either reading - texture alpha
        // alone, or texture alpha times diffuse alpha - the shape's own alpha
        // is what reaches the blend.
        mat->SetTextureBlendMode(VXTEXTUREBLEND_MODULATEALPHA);
        mat->SetTextureAddressMode(VXTEXTURE_ADDRESSCLAMP);
        mat->SetTextureMinMode(VXTEXTUREFILTER_LINEAR);
        mat->SetTextureMagMode(VXTEXTUREFILTER_LINEAR);
        ent->SetMaterial(mat);
    }

    TexQuad q;
    q.ent = ent;
    q.mat = mat;
    g_TexQuads.push_back(q);
    return &g_TexQuads.back();
}

TexQuad* AcquireTexQuad() {
    if (g_TexQuadNext < g_TexQuads.size()) return &g_TexQuads[g_TexQuadNext++];
    TexQuad* q = CreateTexQuad();
    if (q == nullptr) return nullptr;
    ++g_TexQuadNext;
    return q;
}

TextElem* AcquireText() {
    if (g_TextNext < g_Texts.size()) return &g_Texts[g_TextNext++];

    if (g_Bml == nullptr) return nullptr;
    if (CurrentLevel() == nullptr) return nullptr;

    // BGui::Text's constructor reads the player render context (to derive its
    // default font height) without a null check, so refuse to create one until
    // that context exists. Retried on the next frame.
    CKContext* ctx = g_Bml->GetCKContext();
    if (ctx == nullptr || ctx->GetPlayerRenderContext() == nullptr) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "RK_Text_%u", static_cast<unsigned>(g_Texts.size()));

    // The constructor also picks up the game font BGui resolved during BML's
    // own start-up, which is why the facade must not call InitMaterials early
    // (see Compat::Init).
    BGui::Text* t = new BGui::Text(name);
    if (t == nullptr) return nullptr;
    t->SetAlignment(static_cast<CKSPRITETEXT_ALIGNMENT>(CKSPRITETEXT_HCENTER | CKSPRITETEXT_VCENTER));
    t->SetVisible(false);

    TextElem e;
    e.text = t;
    g_Texts.push_back(e);
    ++g_TextNext;
    return &g_Texts.back();
}

// ===========================================================================
// Hot-spot probe
//
// Create a throwaway entity, place it with exactly the two calls BGui uses, and
// read the resulting homogeneous rect back. That settles centre vs top-left
// without a guess or a second build.
// ===========================================================================
void ProbeHotSpot() {
    g_Probed = true;
    if (g_Bml == nullptr) return;
    CKContext* ctx = g_Bml->GetCKContext();
    CKLevel* level = CurrentLevel();
    CKRenderContext* rc = g_Bml->GetRenderContext();
    if (ctx == nullptr || level == nullptr || rc == nullptr) return;

    CK2dEntity* probe = static_cast<CK2dEntity*>(ctx->CreateObject(CKCID_2DENTITY, "RK_Probe"));
    if (probe == nullptr) return;

    probe->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
    level->AddObject(probe);
    probe->SetHomogeneousCoordinates();
    probe->EnableClipToCamera(FALSE);
    probe->EnableRatioOffset(FALSE);
    probe->SetZOrder(-1000);
    probe->Show(CKHIDE);
    probe->SetPosition(Vx2DVector(0.5f, 0.5f), TRUE);
    probe->SetSize(Vx2DVector(0.2f, 0.2f), TRUE);

    VxRect r;
    r.Clear();
    const CKERROR err = probe->GetHomogeneousRect(r);
    const float width = r.right - r.left;
    const float height = r.bottom - r.top;

    if (err == CK_OK && width > 0.01f && std::fabs(width - 0.2f) < 0.02f) {
        const float centreX = (r.left + r.right) * 0.5f;
        g_HotSpotFactor = (std::fabs(centreX - 0.5f) < 0.02f) ? 0.5f : 0.0f;
    }

    if (g_Log != nullptr) {
        g_Log->Info("[RainingKeys/rk2d] probe: homRect=(%.3f,%.3f,%.3f,%.3f) err=%d -> SetPosition means %s",
                    r.left, r.top, r.right, r.bottom, static_cast<int>(err),
                    (g_HotSpotFactor > 0.25f) ? "CENTRE" : "TOP-LEFT");
    }

    if (ctx->GetCurrentLevel() != nullptr) {
        ctx->DestroyObject(CKOBJID(probe));
    }
}

// Same question for the sprite class the text uses. Measured with an actual
// surface (Create) because a sprite's rect may not settle before it has one.
void ProbeTextHotSpot() {
    if (g_Bml == nullptr) return;
    CKContext* ctx = g_Bml->GetCKContext();
    CKLevel* level = CurrentLevel();
    if (ctx == nullptr || level == nullptr) return;

    CKSpriteText* probe =
        static_cast<CKSpriteText*>(ctx->CreateObject(CKCID_SPRITETEXT, "RK_ProbeText"));
    if (probe == nullptr) return;

    probe->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
    level->AddObject(probe);
    probe->SetHomogeneousCoordinates();
    probe->EnableClipToCamera(FALSE);
    probe->EnableRatioOffset(FALSE);
    probe->SetZOrder(-1000);
    probe->Show(CKHIDE);
    probe->Create(64, 32, 32);
    probe->SetPosition(Vx2DVector(0.5f, 0.5f), TRUE);
    probe->SetSize(Vx2DVector(0.2f, 0.2f), TRUE);

    VxRect r;
    r.Clear();
    const CKERROR err = probe->GetHomogeneousRect(r);
    const float width = r.right - r.left;

    bool ok = false;
    if (err == CK_OK && width > 0.01f) {
        const float centreX = (r.left + r.right) * 0.5f;
        const float centreY = (r.top + r.bottom) * 0.5f;
        // Only trust it when the rect really is the 0.2 we asked for, otherwise
        // the sprite reports something unrelated and the generic default stands.
        if (std::fabs(width - 0.2f) < 0.02f) {
            g_TextHotSpotFactor = (std::fabs(centreX - 0.5f) < 0.02f) ? 0.5f : 0.0f;
            ok = true;
        } else if (std::fabs(centreY - 0.5f) < 0.02f) {
            g_TextHotSpotFactor = 0.5f;
            ok = true;
        }
    }

    if (g_Log != nullptr) {
        g_Log->Info("[RainingKeys/rk2d] text probe: homRect=(%.3f,%.3f,%.3f,%.3f) err=%d trusted=%d -> SetPosition means %s",
                    r.left, r.top, r.right, r.bottom, static_cast<int>(err), ok ? 1 : 0,
                    (g_TextHotSpotFactor > 0.25f) ? "CENTRE" : "TOP-LEFT");
    }

    if (ctx->GetCurrentLevel() != nullptr) {
        ctx->DestroyObject(CKOBJID(probe));
    }
}

// ===========================================================================
// Texture factory
//
// Everything drawn with an outline or a fade is rasterised here once and then
// stretched to the rectangle the command asked for. Shapes are stored in the
// top-left corner of a power-of-two canvas and addressed with a source rect:
// D3D8 (which is what Ballance renders through) wants power-of-two textures,
// and texture coordinates are normalised anyway.
// ===========================================================================
enum TexKind {
    TEX_ROUND_FILL = 0,
    TEX_ROUND_RING,
    TEX_RAMP_U_ASC,
    TEX_RAMP_U_DESC,
    TEX_RAMP_V_ASC,
    TEX_RAMP_V_DESC
};

struct TexEntry {
    CKTexture* tex = nullptr;
    int kind = TEX_ROUND_FILL;
    int qw = 0, qh = 0;   // shape size inside the canvas, at the top-left
    int canvas = 0;       // canvas edge, power of two
    int radiusQ = 0;      // key: corner radius, in 1/64 of the shorter side
    int thickQ = 0;       // key: outline thickness, in half pixels
    int alphaQ = 0;       // key: baked alpha level
};

std::vector<TexEntry> g_TexCache;

// Set once, by ProbeTextureUpload(): which upload route works, and whether the
// engine stores image rows bottom-up. A silent vertical flip would reverse the
// vertical fade ramps and produce no error at all, hence the probe.
bool g_TexProbed = false;

// One-off dumps of the first few generated shapes, so the rasterised mask and
// its alpha channel can be inspected as files rather than inferred.
int g_DumpedShape = 0;
int g_DumpedRing = 0;
int g_DumpedRamp = 0;

inline int Pow2Ceil(int v) {
    int p = 16;
    while (p < v && p < 512) p <<= 1;
    return p;
}

inline int QuantSize(float v) {
    int q = static_cast<int>(v + 0.5f);
    q = ((q + MASK_QUANT_PX - 1) / MASK_QUANT_PX) * MASK_QUANT_PX;
    if (q < MASK_QUANT_PX) q = MASK_QUANT_PX;
    if (q > 512) q = 512;
    return q;
}

inline int QuantAlphaLevel(float a) {
    int l = static_cast<int>(a * (ALPHA_LEVELS - 1) / 255.0f + 0.5f);
    if (l < 0) l = 0;
    if (l > ALPHA_LEVELS - 1) l = ALPHA_LEVELS - 1;
    return l;
}

inline float LevelToAlpha(int level) {
    return static_cast<float>(level) * 255.0f / static_cast<float>(ALPHA_LEVELS - 1);
}

inline int QuantRatio(float rr) {
    int q = static_cast<int>(rr * 64.0f + 0.5f);
    return (q < 0) ? 0 : (q > 64 ? 64 : q);
}

inline int QuantThick(float t) {
    int q = static_cast<int>(t * 2.0f + 0.5f);
    return (q < 1) ? 1 : (q > 64 ? 64 : q);
}

inline int MaskShift(CKDWORD m) {
    if (m == 0) return 0;
    int s = 0;
    while ((m & 1u) == 0 && s < 31) {
        m >>= 1;
        ++s;
    }
    return s;
}

// Coverage of a rounded rectangle at a pixel centre, 1 pixel of antialiasing.
// Signed distance to the shape: negative inside.
inline float RoundedSdf(float px, float py, float w, float h, float r) {
    const float qx = std::fabs(px - w * 0.5f) - (w * 0.5f - r);
    const float qy = std::fabs(py - h * 0.5f) - (h * 0.5f - r);
    const float ax = (qx > 0.0f) ? qx : 0.0f;
    const float ay = (qy > 0.0f) ? qy : 0.0f;
    const float outside = std::sqrt(ax * ax + ay * ay);
    const float inside = (qx > qy) ? qx : qy;
    return outside + ((inside < 0.0f) ? inside : 0.0f) - r;
}

inline float Coverage(float sdf) {
    float c = 0.5f - sdf;
    return (c < 0.0f) ? 0.0f : (c > 1.0f ? 1.0f : c);
}

// ---------------------------------------------------------------------------
// Uploading pixels
//
// Two routes, chosen once by probing the engine:
//
//   BULK      GetImageDesc + LockSurfacePtr gives the raw buffer. Fast, but it
//             needs the channel masks and the row order, both of which are
//             properties of the rasteriser - so the probe writes a known corner
//             and reads it back with GetPixel before trusting it.
//   SETPIXEL  CKBitmapData::SetPixel, which is format agnostic. Slower, so it
//             is the fallback rather than the default.
//
// Whichever is chosen is latched. The first attempt at this used only BULK and
// gave up per-call without latching, so every frame re-created and destroyed a
// texture for every shape: that is what cost two thirds of the frame rate, on
// top of silently degrading every rounded corner and gradient to a flat quad.
// ---------------------------------------------------------------------------
enum TexUploadMode { TEXU_UNKNOWN = 0, TEXU_BULK, TEXU_SETPIXEL, TEXU_NONE };

TexUploadMode g_TexUpload = TEXU_UNKNOWN;
int g_TexFlipV = 0;

inline CKDWORD VxPix(int r, int g, int b, int a) {
    return static_cast<CKDWORD>(RGBAITOCOLOR(r, g, b, a));
}

bool UploadBulk(CKTexture* tex, int w, int h, const unsigned char* rgba) {
    VxImageDescEx desc;
    if (!tex->GetImageDesc(desc)) return false;
    if (desc.BitsPerPixel != 32 || desc.BytesPerLine <= 0) return false;

    int slot = 0;
    BYTE* base = tex->LockSurfacePtr(slot);
    if (base == nullptr) {
        slot = -1;
        base = tex->LockSurfacePtr(slot);
    }
    if (base == nullptr) return false;

    const DWORD mr = (desc.RedMask != 0) ? desc.RedMask : 0x00FF0000u;
    const DWORD mg = (desc.GreenMask != 0) ? desc.GreenMask : 0x0000FF00u;
    const DWORD mb = (desc.BlueMask != 0) ? desc.BlueMask : 0x000000FFu;
    const DWORD ma = (desc.AlphaMask != 0) ? desc.AlphaMask : 0xFF000000u;
    const int sr = MaskShift(mr), sg = MaskShift(mg), sb = MaskShift(mb), sa = MaskShift(ma);

    CKDWORD* words = reinterpret_cast<CKDWORD*>(base);
    const int strideWords = desc.BytesPerLine / 4;
    for (int y = 0; y < h; ++y) {
        const int dy = g_TexFlipV ? (h - 1 - y) : y;
        CKDWORD* row = words + static_cast<size_t>(dy) * strideWords;
        const unsigned char* src = rgba + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x] = (static_cast<CKDWORD>(src[x * 4 + 0]) << sr) |
                     (static_cast<CKDWORD>(src[x * 4 + 1]) << sg) |
                     (static_cast<CKDWORD>(src[x * 4 + 2]) << sb) |
                     (static_cast<CKDWORD>(src[x * 4 + 3]) << sa);
        }
    }
    tex->ReleaseSurfacePtr(slot);
    return true;
}

bool UploadByPixel(CKTexture* tex, int w, int h, const unsigned char* rgba) {
    // Fail fast on the very first pixel so a broken path costs nothing.
    const unsigned char* p0 = rgba;
    if (!tex->SetPixel(0, 0, VxPix(p0[0], p0[1], p0[2], p0[3]), 0)) return false;

    for (int y = 0; y < h; ++y) {
        const unsigned char* src = rgba + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = src + x * 4;
            tex->SetPixel(x, y, VxPix(p[0], p[1], p[2], p[3]), 0);
        }
    }
    return true;
}

bool UploadTexture(CKTexture* tex, int w, int h, const unsigned char* rgba) {
    if (tex == nullptr || rgba == nullptr) return false;
    if (g_TexUpload == TEXU_BULK) return UploadBulk(tex, w, h, rgba);
    if (g_TexUpload == TEXU_SETPIXEL) return UploadByPixel(tex, w, h, rgba);
    return false;   // TEXU_UNKNOWN / TEXU_NONE: the probe owns the decision
}

// Reads a pixel back through the descriptor's own channel masks.
bool ReadPixel(CKTexture* tex, int x, int y, VxImageDescEx& desc, int& r, int& g, int& b, int& a) {
    if (!tex->GetImageDesc(desc) || desc.BitsPerPixel != 32) return false;
    const DWORD mr = (desc.RedMask != 0) ? desc.RedMask : 0x00FF0000u;
    const DWORD mg = (desc.GreenMask != 0) ? desc.GreenMask : 0x0000FF00u;
    const DWORD mb = (desc.BlueMask != 0) ? desc.BlueMask : 0x000000FFu;
    const DWORD ma = (desc.AlphaMask != 0) ? desc.AlphaMask : 0xFF000000u;
    const CKDWORD px = tex->GetPixel(x, y, 0);
    r = static_cast<int>((px & mr) >> MaskShift(mr));
    g = static_cast<int>((px & mg) >> MaskShift(mg));
    b = static_cast<int>((px & mb) >> MaskShift(mb));
    a = static_cast<int>((px & ma) >> MaskShift(ma));
    return true;
}

// An asymmetric reference image. Every channel varies with a different phase,
// so a mismatch identifies WHICH channel went where, not merely that one did.
// The alpha ramp is deliberately not constant: a texture whose alpha is dropped
// on the way to the video surface is indistinguishable from a correct one if
// every texel is opaque.
void MakeReferenceImage(int w, int h, std::vector<unsigned char>& buf) {
    buf.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &buf[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<unsigned char>(16 + (x * 3 + y) % 224);   // R: mostly x
            p[1] = static_cast<unsigned char>(16 + (y * 3 + x) % 224);   // G: mostly y
            p[2] = static_cast<unsigned char>(200 - (x + y) % 180);      // B: falling
            p[3] = static_cast<unsigned char>(32 + ((x / 4 + y / 4) % 6) * 32);  // A: 32..192
        }
    }
}

// How many of the sampled texels fail to read back as written.
int CountReferenceMismatches(CKTexture* tex, int w, int h, const unsigned char* buf,
                             int& firstX, int& firstY) {
    static const int kPts[6][2] = { {1, 1}, {7, 3}, {20, 9}, {33, 41}, {62, 22}, {61, 60} };
    VxImageDescEx desc;
    int bad = 0;
    firstX = firstY = -1;
    for (int i = 0; i < 6; ++i) {
        const int x = kPts[i][0] < w ? kPts[i][0] : (w - 1);
        const int y = kPts[i][1] < h ? kPts[i][1] : (h - 1);
        int r = 0, g = 0, b = 0, a = 0;
        if (!ReadPixel(tex, x, y, desc, r, g, b, a)) return -1;
        const unsigned char* want = buf + (static_cast<size_t>(y) * w + x) * 4;
        const int d = std::abs(r - want[0]) + std::abs(g - want[1]) + std::abs(b - want[2]) +
                      std::abs(a - want[3]);
        if (d > 6) {
            ++bad;
            if (firstX < 0) {
                firstX = x;
                firstY = y;
                if (g_Log != nullptr) {
                    g_Log->Info("[RainingKeys/rk2d]   mismatch at (%d,%d): want %d,%d,%d,%d got %d,%d,%d,%d",
                                x, y, want[0], want[1], want[2], want[3], r, g, b, a);
                }
            }
        }
    }
    return bad;
}

void DumpTexture(CKTexture* tex, const char* tag) {
    if (tex == nullptr) return;

    // Only dump textures the engine is happy to save. A 1-pixel-thin bitmap was
    // the shape that crashed, so thin textures are skipped even though nothing
    // generates them any more.
    const int w = tex->GetWidth();
    const int h = tex->GetHeight();
    if (w < 8 || h < 8) {
        if (g_Log != nullptr) {
            g_Log->Info("[RainingKeys/rk2d]   (skipped dump of %dx%d %s)", w, h, tag);
        }
        return;
    }

    char path[192];
    std::snprintf(path, sizeof(path), "..\\ModLoader\\Cache\\RK_%s.bmp", tag);
    tex->SaveImage(path, 0, FALSE);
    std::snprintf(path, sizeof(path), "..\\ModLoader\\Cache\\RK_%s_alpha.bmp", tag);
    tex->SaveImageAlpha(path, 0);
    if (g_Log != nullptr) {
        g_Log->Info("[RainingKeys/rk2d]   dumped RK_%s.bmp / RK_%s_alpha.bmp (%dx%d)", tag, tag, w, h);
    }
}

void LogTextureDesc(const char* which, VxImageDescEx& d, bool ok) {
    if (g_Log == nullptr) return;
    if (!ok) {
        g_Log->Info("[RainingKeys/rk2d] %s desc: unavailable", which);
        return;
    }
    g_Log->Info("[RainingKeys/rk2d] %s desc: %dx%d bpp=%d pitch=%d masks R=%08X G=%08X B=%08X A=%08X",
                which, d.Width, d.Height, d.BitsPerPixel, d.BytesPerLine,
                static_cast<unsigned>(d.RedMask), static_cast<unsigned>(d.GreenMask),
                static_cast<unsigned>(d.BlueMask), static_cast<unsigned>(d.AlphaMask));
}

// Decides once how texture pixels get into the engine, and records everything
// needed to explain the result if the choice turns out to be wrong.
void ProbeTextureUpload() {
    if (g_TexUpload != TEXU_UNKNOWN) return;
    g_TexUpload = TEXU_NONE;   // pessimistic until a route proves itself
    g_TexProbed = true;

    CKContext* ctx = (g_Bml != nullptr) ? g_Bml->GetCKContext() : nullptr;
    CKLevel* level = CurrentLevel();
    if (ctx == nullptr || level == nullptr) return;

    const int K = 64;
    CKTexture* probe = static_cast<CKTexture*>(ctx->CreateObject(CKCID_TEXTURE, "RK_ProbeTex"));
    if (probe == nullptr) return;
    probe->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
    level->AddObject(probe);
    if (!probe->Create(K, K, 32)) {
        ctx->DestroyObject(CKOBJID(probe));
        return;
    }

    std::vector<unsigned char> img;
    MakeReferenceImage(K, K, img);

    VxImageDescEx sysDesc, vidDesc;
    LogTextureDesc("system", sysDesc, probe->GetImageDesc(sysDesc) != 0);
    LogTextureDesc("video", vidDesc, probe->GetVideoTextureDesc(vidDesc) != 0);
    if (g_Log != nullptr) {
        g_Log->Info("[RainingKeys/rk2d] desired video format = %d",
                    static_cast<int>(probe->GetDesiredVideoFormat()));
    }

    const char* how = "none";
    int x = -1, y = -1;

    // Route 1: raw buffer, with the row order resolved by trying both.
    if (UploadBulk(probe, K, K, img.data())) {
        int bad = -1;
        for (int flip = 0; flip <= 1; ++flip) {
            g_TexFlipV = flip;
            if (!UploadBulk(probe, K, K, img.data())) continue;
            bad = CountReferenceMismatches(probe, K, K, img.data(), x, y);
            if (bad == 0) {
                g_TexUpload = TEXU_BULK;
                how = flip ? "bulk/bottom-up" : "bulk/top-down";
                break;
            }
        }
        if (g_TexUpload == TEXU_NONE && g_Log != nullptr) {
            g_Log->Info("[RainingKeys/rk2d] bulk upload disagrees with GetPixel (%d/6 samples)", bad);
        }
        if (g_TexUpload == TEXU_NONE) g_TexFlipV = 0;
        DumpTexture(probe, "probe_bulk");
        // Leave the texture holding the raw-buffer result for the A/B dump.
        g_TexFlipV = 0;
        UploadBulk(probe, K, K, img.data());
    }

    // Route 2: per pixel, format agnostic.
    if (g_TexUpload == TEXU_NONE) {
        if (UploadByPixel(probe, K, K, img.data())) {
            const int bad = CountReferenceMismatches(probe, K, K, img.data(), x, y);
            if (bad == 0) {
                g_TexUpload = TEXU_SETPIXEL;
                g_TexFlipV = 0;
                how = "setpixel";
            } else if (g_Log != nullptr) {
                g_Log->Info("[RainingKeys/rk2d] setpixel upload disagrees too (%d/6 samples)", bad);
            }
            DumpTexture(probe, "probe_setpixel");
        }
    }

    if (g_Log != nullptr) {
        if (g_TexUpload == TEXU_NONE) {
            g_Log->Warn("[RainingKeys/rk2d] no working texture upload path; rounded corners "
                        "and gradients fall back to flat quads");
        } else {
            g_Log->Info("[RainingKeys/rk2d] texture upload path: %s", how);
        }
    }

    if (ctx->GetCurrentLevel() != nullptr) {
        ctx->DestroyObject(CKOBJID(probe));
    }
}

CKTexture* CreateTexture(int w, int h, const char* baseName) {
    if (g_Bml == nullptr) return nullptr;
    CKContext* ctx = g_Bml->GetCKContext();
    CKLevel* level = CurrentLevel();
    if (ctx == nullptr || level == nullptr) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "%s_%u", baseName, static_cast<unsigned>(g_TexCache.size()));

    CKTexture* tex = static_cast<CKTexture*>(ctx->CreateObject(CKCID_TEXTURE, name));
    if (tex == nullptr) return nullptr;
    tex->ModifyObjectFlags(CK_OBJECT_NOTTOBELISTEDANDSAVED, 0);
    level->AddObject(tex);

    // The rasteriser's default target is _16_ARGB1555 - one single bit of
    // alpha. Every shape alpha we bake in (0..187) collapses to "opaque", which
    // is why the boxes lost their translucency while keeping their rounded
    // silhouette. Ask for 32-bit ARGB, the same layout the system image already
    // uses, so the alpha channel survives the upload.
    tex->SetDesiredVideoFormat(_32_ARGB8888);

    if (!tex->Create(w, h, 32)) {
        ctx->DestroyObject(CKOBJID(tex));
        return nullptr;
    }
    return tex;
}

const TexEntry* FindTexture(int kind, int qw, int qh, int radiusQ, int thickQ, int alphaQ) {
    for (size_t i = 0; i < g_TexCache.size(); ++i) {
        const TexEntry& e = g_TexCache[i];
        if (e.kind == kind && e.qw == qw && e.qh == qh && e.radiusQ == radiusQ &&
            e.thickQ == thickQ && e.alphaQ == alphaQ) {
            return &e;
        }
    }
    return nullptr;
}

// Rounded fill or rounded outline ring, addressed through the source rect.
//
// The shape is rasterised at a size snapped up to the grid and then stretched
// back down to the exact rectangle. Both dimensions are scaled by the SAME
// factor so the aspect ratio survives, which is what makes the stretched radius
// come out exactly right: the radius is a fraction of the shorter side, and
// scaling the shorter side by k scales the radius by k too.
const TexEntry* GetShapeTexture(int kind, float w, float h, float radius, float thickness,
                                float alpha) {
    if (!g_TexProbed) ProbeTextureUpload();
    if (g_TexUpload == TEXU_NONE) return nullptr;   // latched: never retry per frame

    const float shorter = (w < h) ? w : h;
    if (shorter <= 0.0f) return nullptr;

    const float k = static_cast<float>(QuantSize(shorter)) / shorter;
    int qw = static_cast<int>(w * k + 0.5f);
    int qh = static_cast<int>(h * k + 0.5f);
    if (qw < MASK_QUANT_PX) qw = MASK_QUANT_PX;
    if (qh < MASK_QUANT_PX) qh = MASK_QUANT_PX;
    if (qw > 512) qw = 512;
    if (qh > 512) qh = 512;

    const int radiusQ = QuantRatio(radius / shorter);
    const int thickQ = (kind == TEX_ROUND_RING) ? QuantThick(thickness) : 0;
    const int alphaQ = (kind == TEX_ROUND_FILL) ? QuantAlphaLevel(alpha) : (ALPHA_LEVELS - 1);

    if (const TexEntry* hit = FindTexture(kind, qw, qh, radiusQ, thickQ, alphaQ)) {
        return hit;
    }
    if (g_TexCache.size() >= TEX_CACHE_MAX) return nullptr;

    const int canvas = Pow2Ceil((qw > qh) ? qw : qh);
    CKTexture* tex = CreateTexture(canvas, canvas, (kind == TEX_ROUND_RING) ? "RK_Ring" : "RK_Round");
    if (tex == nullptr) return nullptr;

    const float r = static_cast<float>(qw < qh ? qw : qh) * (radiusQ / 64.0f);
    const float t = static_cast<float>(thickQ) * 0.5f;
    const float aOut = (kind == TEX_ROUND_FILL) ? LevelToAlpha(alphaQ) : 255.0f;

    std::vector<unsigned char> buf(static_cast<size_t>(canvas) * canvas * 4, 0);
    for (int y = 0; y < qh; ++y) {
        for (int x = 0; x < qw; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float d = RoundedSdf(px, py, static_cast<float>(qw), static_cast<float>(qh), r);

            float cov;
            if (kind == TEX_ROUND_FILL) {
                cov = Coverage(d);
            } else {
                cov = Coverage(d) - Coverage(d + t);   // annulus of thickness t
            }
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            const int alpha = static_cast<int>(cov * aOut + 0.5f);
            if (alpha <= 0) continue;
            unsigned char* p = &buf[(static_cast<size_t>(y) * canvas + x) * 4];
            p[0] = 255; p[1] = 255; p[2] = 255;   // white: the colour comes from the diffuse
            p[3] = static_cast<unsigned char>(alpha > 255 ? 255 : alpha);
        }
    }

    if (!UploadTexture(tex, canvas, canvas, buf.data())) {
        if (g_Bml) g_Bml->GetCKContext()->DestroyObject(CKOBJID(tex));
        return nullptr;
    }

    TexEntry e;
    e.tex = tex;
    e.kind = kind;
    e.qw = qw;
    e.qh = qh;
    e.canvas = canvas;
    e.radiusQ = radiusQ;
    e.thickQ = thickQ;
    e.alphaQ = alphaQ;
    g_TexCache.push_back(e);

    if (kind == TEX_ROUND_RING) {
        if (!g_DumpedRing) {
            g_DumpedRing = 1;
            DumpTexture(tex, "ring");
        }
    } else if (g_DumpedShape < 3) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "fill%d", g_DumpedShape);
        ++g_DumpedShape;
        DumpTexture(tex, tag);
    }

    return &g_TexCache.back();
}

// Linear alpha ramp for the band fade. The ramp is read as "alpha = level * v",
// and the caller crops the sub-range that reproduces BML+'s chord exactly.
const TexEntry* GetRampTexture(int kind, float alpha) {
    if (!g_TexProbed) ProbeTextureUpload();
    if (g_TexUpload == TEXU_NONE) return nullptr;   // latched: never retry per frame

    const int alphaQ = QuantAlphaLevel(alpha);
    const int qw = RAMP_TEXELS;
    const int qh = RAMP_TEXELS;

    if (const TexEntry* hit = FindTexture(kind, qw, qh, 0, 0, alphaQ)) return hit;
    if (g_TexCache.size() >= TEX_CACHE_MAX) return nullptr;

    CKTexture* tex = CreateTexture(qw, qh, "RK_Ramp");
    if (tex == nullptr) return nullptr;

    const float peak = LevelToAlpha(alphaQ);
    const bool alongU = (kind == TEX_RAMP_U_ASC || kind == TEX_RAMP_U_DESC);
    const bool ascending = (kind == TEX_RAMP_U_ASC || kind == TEX_RAMP_V_ASC);

    // Square canvas, ramp along one axis, constant along the other, so the
    // caller can take any sub-range of the ramp and the whole extent of the
    // constant axis.
    std::vector<unsigned char> buf(static_cast<size_t>(qw) * qh * 4, 0);
    for (int i = 0; i < RAMP_TEXELS; ++i) {
        const float v = static_cast<float>(i) / static_cast<float>(RAMP_TEXELS - 1);
        const float f = ascending ? v : (1.0f - v);
        int alpha8 = static_cast<int>(peak * f + 0.5f);
        if (alpha8 < 0) alpha8 = 0;
        if (alpha8 > 255) alpha8 = 255;
        for (int j = 0; j < RAMP_TEXELS; ++j) {
            const size_t idx = alongU ? (static_cast<size_t>(j) * RAMP_TEXELS + i)
                                      : (static_cast<size_t>(i) * RAMP_TEXELS + j);
            unsigned char* p = &buf[idx * 4];
            p[0] = 255; p[1] = 255; p[2] = 255;
            p[3] = static_cast<unsigned char>(alpha8);
        }
    }

    if (!UploadTexture(tex, qw, qh, buf.data())) {
        if (g_Bml) g_Bml->GetCKContext()->DestroyObject(CKOBJID(tex));
        return nullptr;
    }

    TexEntry e;
    e.tex = tex;
    e.kind = kind;
    e.qw = qw;
    e.qh = qh;
    e.canvas = RAMP_TEXELS;
    e.alphaQ = alphaQ;
    g_TexCache.push_back(e);

    if (g_DumpedRamp < 4) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "ramp%d", g_DumpedRamp);
        ++g_DumpedRamp;
        DumpTexture(tex, tag);
    }

    return &g_TexCache.back();
}

// ===========================================================================
// Low level drawing
// ===========================================================================
// Queried once per frame from BeginFrame(); every quad used to ask the render
// context for its size again, which is three virtual calls per quad per frame.
void RefreshViewport() {
    if (g_Bml == nullptr) return;
    CKRenderContext* rc = g_Bml->GetRenderContext();
    if (rc == nullptr) return;
    const float rw = static_cast<float>(rc->GetWidth());
    const float rh = static_cast<float>(rc->GetHeight());
    if (rw > 1.0f && rh > 1.0f) {
        g_VpW = rw;
        g_VpH = rh;
    }
}

void ViewportSize(float& w, float& h) {
    w = g_VpW;
    h = g_VpH;
}

void DrawQuad(float x0, float y0, float x1, float y1, ImU32 col, int z) {
    if (ChanA(col) <= 0) return;
    if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;

    Quad* q = AcquireQuad();
    if (q == nullptr || q->ent == nullptr) return;

    float vpW = 1600.0f, vpH = 1200.0f;
    ViewportSize(vpW, vpH);

    const float w = x1 - x0;
    const float h = y1 - y0;

    // Position is the hot spot; shift it so the *rect* lands on [x0,y0,x1,y1]
    // whichever convention the engine turned out to use.
    const float px = x0 + w * g_HotSpotFactor;
    const float py = y0 + h * g_HotSpotFactor;

    CK2dEntity* e = q->ent;

    if (!q->hasRect || q->w != w || q->h != h) {
        e->SetSize(Vx2DVector(w / vpW, h / vpH), TRUE);
        q->w = w;
        q->h = h;
    }

    if (!q->hasRect || q->x0 != px || q->y0 != py) {
        e->SetPosition(Vx2DVector(px / vpW, py / vpH), TRUE);
        q->x0 = px;
        q->y0 = py;
    }

    if (q->mat != nullptr && q->col != col) {
        q->mat->SetDiffuse(VxOf(col));
        q->col = col;
    }

    if (q->z != z) {
        e->SetZOrder(z);
        q->z = z;
    }

    if (!q->visible) {
        e->Show(CKSHOW);
        q->visible = true;
    }
    q->hasRect = true;
}

void FillRect(float x0, float y0, float x1, float y1, ImU32 col, int z) {
    DrawQuad(x0, y0, x1, y1, col, z);
}

// Textured element. The texture carries a white RGB with the shape's alpha
// baked in, so the diffuse supplies the colour and the alpha is left at 255.
void DrawTexQuad(float x0, float y0, float x1, float y1, CKTexture* tex, const VxRect& src,
                 ImU32 col, int z) {
    if (tex == nullptr) return;
    if (ChanA(col) <= 0) return;
    if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;

    TexQuad* q = AcquireTexQuad();
    if (q == nullptr || q->ent == nullptr || q->mat == nullptr) return;

    float vpW = 1600.0f, vpH = 1200.0f;
    ViewportSize(vpW, vpH);

    const float w = x1 - x0;
    const float h = y1 - y0;
    const float px = x0 + w * g_HotSpotFactor;
    const float py = y0 + h * g_HotSpotFactor;

    CK2dEntity* e = q->ent;

    if (!q->hasRect || q->w != w || q->h != h) {
        e->SetSize(Vx2DVector(w / vpW, h / vpH), TRUE);
        q->w = w;
        q->h = h;
    }
    if (!q->hasRect || q->x0 != px || q->y0 != py) {
        e->SetPosition(Vx2DVector(px / vpW, py / vpH), TRUE);
        q->x0 = px;
        q->y0 = py;
    }
    if (!q->hasSrc || q->src.left != src.left || q->src.top != src.top ||
        q->src.right != src.right || q->src.bottom != src.bottom) {
        e->SetSourceRect(src);
        q->src = src;
        q->hasSrc = true;
    }
    if (q->tex != tex) {
        q->mat->SetTexture(tex);
        q->tex = tex;
    }
    if (q->col != col) {
        q->mat->SetDiffuse(VxOfTint(col));
        q->col = col;
    }
    if (q->z != z) {
        e->SetZOrder(z);
        q->z = z;
    }
    if (!q->visible) {
        e->Show(CKSHOW);
        q->visible = true;
    }
    q->hasRect = true;
}

// Source rect for a sub-range of a ramp texture, offset by half a texel so the
// endpoints land on texel centres under linear filtering.
void RampCrop(float a, float b, float& outA, float& outB) {
    const float n = static_cast<float>(RAMP_TEXELS);
    outA = (a * (n - 1.0f) + 0.5f) / n;
    outB = (b * (n - 1.0f) + 0.5f) / n;
    if (outA < 0.0f) outA = 0.0f;
    if (outA > 1.0f) outA = 1.0f;
    if (outB < 0.0f) outB = 0.0f;
    if (outB > 1.0f) outB = 1.0f;
    if (outB < outA) outB = outA;
}

// Rounded filled rectangle: one generated mask instead of a plain box, so the
// corners are antialiased and the radius is a true fraction of the shorter side.
void RoundedFillRect(float x0, float y0, float x1, float y1, ImU32 col, float rounding, int z) {
    const float w = x1 - x0;
    const float h = y1 - y0;
    if (w < 0.01f || h < 0.01f) return;
    if (rounding < 0.75f) {
        FillRect(x0, y0, x1, y1, col, z);
        return;
    }

    const TexEntry* e = GetShapeTexture(TEX_ROUND_FILL, w, h, rounding, 0.0f,
                                        static_cast<float>(ChanA(col)));
    if (e == nullptr) {
        FillRect(x0, y0, x1, y1, col, z);   // out of cache: fall back to a square box
        return;
    }
    const float inv = 1.0f / static_cast<float>(e->canvas);
    const VxRect src(0.0f, 0.0f, e->qw * inv, e->qh * inv);
    DrawTexQuad(x0, y0, x1, y1, e->tex, src, col, z);
}

// Rounded outline: one annulus mask. The ring is opaque in the mask, so the
// diffuse supplies the colour and no per-alpha texture variant is needed.
void OutlineRect(float x0, float y0, float x1, float y1, ImU32 col, float thickness, int z);

void RoundedOutlineRect(float x0, float y0, float x1, float y1, ImU32 col, float rounding,
                        float thickness, int z) {
    const float w = x1 - x0;
    const float h = y1 - y0;
    if (w <= 0.0f || h <= 0.0f) return;
    if (rounding < 0.75f) {
        OutlineRect(x0, y0, x1, y1, col, thickness, z);
        return;
    }

    const TexEntry* e = GetShapeTexture(TEX_ROUND_RING, w, h, rounding, thickness, 255.0f);
    if (e == nullptr) {
        OutlineRect(x0, y0, x1, y1, col, thickness, z);
        return;
    }
    const float inv = 1.0f / static_cast<float>(e->canvas);
    const VxRect src(0.0f, 0.0f, e->qw * inv, e->qh * inv);
    DrawTexQuad(x0, y0, x1, y1, e->tex, src, col, z);
}

// Four bands inset inside the rect, mirroring what the previous legacy build
// did. Square corners for now; step 2 replaces this with a generated ring mask.
void OutlineRect(float x0, float y0, float x1, float y1, ImU32 col, float thickness, int z) {
    const float t = thickness;
    if (t <= 0.0f) return;
    if (x1 - x0 <= 0.0f || y1 - y0 <= 0.0f) return;
    if (2.0f * t >= (x1 - x0) || 2.0f * t >= (y1 - y0)) {
        FillRect(x0, y0, x1, y1, col, z);
        return;
    }
    FillRect(x0, y0, x1, y0 + t, col, z);
    FillRect(x0, y1 - t, x1, y1, col, z);
    FillRect(x0, y0 + t, x0 + t, y1 - t, col, z);
    FillRect(x1 - t, y0 + t, x1, y1 - t, col, z);
}

// The mod only ever builds two-stop gradients (it replicates one colour per
// edge), so the axis is recovered from which corners agree. Anything else is
// not expressible as a linear ramp and degrades to the top-left colour; nothing
// in RainingKeys produces one.
//
// The two-stop case is reproduced EXACTLY, not approximated: BML+ hands
// AddRectFilledMultiColor two endpoint colours and ImGui interpolates linearly
// between them, so cropping a linear alpha ramp by the two ALPHA FRACTIONS (not
// by distance) and stretching that crop over the rectangle yields the identical
// ramp. One sprite replaces the whole stack of constant-alpha strips.
void GradientRect(float x0, float y0, float x1, float y1, ImU32 tl, ImU32 tr, ImU32 br,
                  ImU32 bl, int z) {
    bool vertical = false;
    ImU32 cA = tl, cB = bl;
    if (tl == tr && bl == br) {
        vertical = true;
        cA = tl;
        cB = bl;
    } else if (tl == bl && tr == br) {
        vertical = false;
        cA = tl;
        cB = tr;
    } else {
        FillRect(x0, y0, x1, y1, tl, z);
        return;
    }

    if (cA == cB) {
        FillRect(x0, y0, x1, y1, cA, z);
        return;
    }

    const float aA = static_cast<float>(ChanA(cA));
    const float aB = static_cast<float>(ChanA(cB));
    const float peak = (aA > aB) ? aA : aB;
    if (peak <= 0.0f) return;

    int kind;
    float lo, hi;
    if (aA <= aB) {
        kind = vertical ? TEX_RAMP_V_ASC : TEX_RAMP_U_ASC;
        lo = aA / peak;
        hi = 1.0f;
    } else {
        kind = vertical ? TEX_RAMP_V_DESC : TEX_RAMP_U_DESC;
        lo = 0.0f;
        hi = 1.0f - aB / peak;
    }

    const TexEntry* e = GetRampTexture(kind, peak);
    if (e == nullptr) {
        // Out of cache: fall back to a single flat quad at the stronger end.
        FillRect(x0, y0, x1, y1, (aA >= aB) ? cA : cB, z);
        return;
    }

    float s0 = 0.0f, s1 = 1.0f;
    RampCrop(lo, hi, s0, s1);
    VxRect src;
    if (vertical) {
        src.SetCorners(0.0f, s0, 1.0f, s1);
    } else {
        src.SetCorners(s0, 0.0f, s1, 1.0f);
    }

    // The alpha lives in the ramp texture, so the colour passed here is only an
    // RGB tint - but it still has to be an OPAQUE tint, and of the STRONGER end.
    //
    // DrawTexQuad treats a fully transparent colour as "nothing to draw" and
    // skips the quad, and cA/cB are exactly the two ends of the ramp. Handing it
    // the weaker end therefore dropped the entire fade whenever that end was
    // fully transparent - which is the normal state of a band held at its length
    // limit, where the far end's alpha is 0 by construction. The band then ended
    // in precisely the hard cut the fade exists to remove, and only the ~60ms
    // spent growing into the fade zone (where the far end's alpha is still a
    // small positive number) showed any softening at all.
    //
    // This is why the fault was direction dependent: cA is always the top-left
    // corner, and for Up/Left that corner is the far (weak) end while for
    // Down/Right it is the near (strong) end - so only Up and Left lost their
    // fade, which is what the runtime A/B confirmed.
    //
    // VxOfTint() forces the diffuse alpha to 255 anyway, so nothing about the
    // render changes apart from the quad no longer being thrown away.
    const ImU32 tint = (aA >= aB) ? cA : cB;
    DrawTexQuad(x0, y0, x1, y1, e->tex, src,
                PackIm(ChanR(tint), ChanG(tint), ChanB(tint), 255), z);
}

// The mod's friendly key names for the four arrow keys are U+2190..U+2193, but
// CKSpriteText draws through the game's own font enumeration
// (CKPGUID_FONTNAME), which has no glyphs for them: each UTF-8 byte is rendered
// as one replacement box, which is exactly the mojibake the first legacy build
// showed. Replace those code points with words. This is a glyph-coverage limit
// of the host, so it belongs in the facade, not in the shared drawing code.
const char* HostSafeText(const char* in, char* out, size_t outSize) {
    if (in == nullptr || outSize == 0) return "";
    static const char* const kArrow[4] = { "\xE2\x86\x90", "\xE2\x86\x91",
                                          "\xE2\x86\x92", "\xE2\x86\x93" };
    static const char* const kWord[4] = { "Left", "Up", "Right", "Down" };

    // Fast path: the U+2190..U+2193 range always starts with the same two bytes.
    if (std::strstr(in, "\xE2\x86") == nullptr) return in;

    size_t o = 0;
    for (const char* p = in; *p != '\0';) {
        int hit = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::strncmp(p, kArrow[i], 3) == 0) {
                hit = i;
                break;
            }
        }
        if (hit >= 0) {
            for (const char* w = kWord[hit]; *w != '\0' && o + 1 < outSize; ++w) out[o++] = *w;
            p += 3;
        } else {
            if (o + 1 < outSize) out[o++] = *p;
            ++p;
        }
    }
    out[o] = '\0';
    return out;
}

// `centreX` is the horizontal CENTRE of the text (CalcTextSizeA reports a zero
// width, so the shared code's `x - ts.x * 0.5f` already evaluates to exactly the
// centre it wants). `lineTop` is the top of an ImGui-style line box of height
// `fontSize`, matching ImGui's own top-left text anchor.
void DrawTextElem(float fontSize, float centreX, float lineTop, ImU32 col, const char* text,
                  int z) {
    if (text == nullptr || text[0] == '\0') return;
    if (ChanA(col) <= 0) return;
    if (fontSize < 1.0f) return;

    char safe[128];
    text = HostSafeText(text, safe, sizeof(safe));
    if (text[0] == '\0') return;

    TextElem* e = AcquireText();
    if (e == nullptr || e->text == nullptr) return;

    const size_t len = std::strlen(text);
    float w = fontSize * (static_cast<float>(len) + 1.0f) * TEXT_WIDTH_PER_CHAR;
    const float minW = fontSize * TEXT_MIN_WIDTH_FACTOR;
    const float maxW = fontSize * TEXT_MAX_WIDTH_FACTOR;
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    const float h = fontSize * TEXT_HEIGHT_RATIO;

    float vpW = 1600.0f, vpH = 1200.0f;
    ViewportSize(vpW, vpH);

    BGui::Text* t = e->text;

    // ORDER MATTERS. BGui::Text::SetSize() is ReleaseAllSlots() + Create() +
    // SetSize(), and Create() throws away everything already rasterised into the
    // surface - including the glyphs. So the surface is rebuilt first and both
    // the font and the text are (re)applied afterwards, with their caches
    // invalidated right here. Without this, any size change (i.e. editing
    // Main/Scale) blanked every label for good: the counts reappeared only
    // because their text content kept changing and forced a fresh SetText.
    if (e->w != w || e->h != h) {
        t->SetSize(Vx2DVector(w / vpW, h / vpH));
        e->w = w;
        e->h = h;
        e->px = -1.0f;
        e->content[0] = '\0';
    }

    const int wantPx = static_cast<int>(fontSize * FONT_EM_TO_CAP + 0.5f);
    if (e->px != static_cast<float>(wantPx)) {
        t->SetFont("", wantPx, 400, FALSE, FALSE);
        e->px = static_cast<float>(wantPx);
        if (g_Log != nullptr && !g_LoggedFontOnce) {
            g_LoggedFontOnce = true;
            g_Log->Info("[RainingKeys/rk2d] font: logical=%.1f -> SetFont size=%d (calibration %.4f)",
                        fontSize, wantPx, FONT_EM_TO_CAP);
        }
    }

    if (std::strcmp(e->content, text) != 0) {
        t->SetText(text);
        std::snprintf(e->content, sizeof(e->content), "%s", text);
    }

    const CKDWORD tcol = VxTextColorOf(col);
    if (e->col != tcol) {
        t->SetTextColor(tcol);
        e->col = tcol;
    }

    // The surface is centred on centreX and holds a fontSize-tall line box whose
    // top sits at lineTop; the hot-spot factor then converts that rect into
    // whatever SetPosition actually means on this host.
    const float left = centreX - w * 0.5f;
    const float top = lineTop - (h - fontSize) * 0.5f;
    if (e->cx != left || e->cy != top) {
        const float px = left + w * g_TextHotSpotFactor;
        const float py = top + h * g_TextHotSpotFactor;
        t->SetPosition(Vx2DVector(px / vpW, py / vpH));
        e->cx = left;
        e->cy = top;
    }

    if (e->z != z) {
        t->SetZOrder(z);
        e->z = z;
    }

    if (!e->visible) {
        t->SetVisible(true);
        e->visible = true;
    }
}

void HideQuad(Quad& q) {
    if (q.ent != nullptr && q.visible) {
        q.ent->Show(CKHIDE);
        q.visible = false;
    }
}

void HideTexQuad(TexQuad& q) {
    if (q.ent != nullptr && q.visible) {
        q.ent->Show(CKHIDE);
        q.visible = false;
    }
}

void HideText(TextElem& e) {
    if (e.text != nullptr && e.visible) {
        e.text->SetVisible(false);
        e.visible = false;
    }
}

} // namespace

// ===========================================================================
// ImFont
// ===========================================================================
ImVec2 ImFont::CalcTextSizeA(float size, float /*max_width*/, float /*wrap_width*/,
                             const char* /*text_begin*/) const {
    // See the header: a zero width makes the shared  x - ts.x * 0.5f  centring
    // evaluate to the centre itself, and a height equal to the size keeps the
    // vertical arithmetic identical to ImGui's line height.
    return ImVec2(0.0f, size);
}

// ===========================================================================
// ImDrawList
// ===========================================================================
void ImDrawList::AddRectFilled(const ImVec2& pmin, const ImVec2& pmax, ImU32 col,
                               float rounding, int /*flags*/) {
    Cmd c;
    c.kind = Cmd::FILL;
    c.x0 = pmin.x; c.y0 = pmin.y; c.x1 = pmax.x; c.y1 = pmax.y;
    c.c0 = col;
    c.rounding = rounding;
    g_Cmds.push_back(c);
}

void ImDrawList::AddRect(const ImVec2& pmin, const ImVec2& pmax, ImU32 col,
                         float rounding, int /*flags*/, float thickness) {
    Cmd c;
    c.kind = Cmd::OUTLINE;
    c.x0 = pmin.x; c.y0 = pmin.y; c.x1 = pmax.x; c.y1 = pmax.y;
    c.c0 = col;
    c.rounding = rounding;
    c.thickness = thickness;
    g_Cmds.push_back(c);
}

void ImDrawList::AddRectFilledMultiColor(const ImVec2& pmin, const ImVec2& pmax,
                                         ImU32 col_upr_left, ImU32 col_upr_right,
                                         ImU32 col_bot_right, ImU32 col_bot_left) {
    Cmd c;
    c.kind = Cmd::GRADIENT;
    c.x0 = pmin.x; c.y0 = pmin.y; c.x1 = pmax.x; c.y1 = pmax.y;
    c.c0 = col_upr_left;
    c.c1 = col_upr_right;
    c.c2 = col_bot_right;
    c.c3 = col_bot_left;
    g_Cmds.push_back(c);
}

void ImDrawList::AddText(ImFont* /*font*/, float font_size, const ImVec2& pos, ImU32 col,
                         const char* text_begin) {
    Cmd c;
    c.kind = Cmd::TEXT;
    // Both coordinates are taken straight from ImGui's anchor, which is the
    // top-left of the text's line box. DrawTextElem reads x as the horizontal
    // centre, which is what the shared call sites already compute because
    // CalcTextSizeA reports a zero width.
    c.x0 = pos.x;
    c.y0 = pos.y;
    c.c0 = col;
    c.fontSize = font_size;
    std::snprintf(c.text, sizeof(c.text), "%s", text_begin != nullptr ? text_begin : "");
    g_Cmds.push_back(c);
}

// ===========================================================================
// Entry points
// ===========================================================================
namespace ImGui {

double GetTime() {
    if (g_Bml == nullptr) return 0.0;
    CKTimeManager* tm = g_Bml->GetTimeManager();
    if (tm == nullptr) return 0.0;
    // CKTimeManager reports MILLISECONDS (BML+ re-exports GetTime() as
    // "GetTimeMs"), while ImGui::GetTime() is seconds since start-up. Getting
    // this wrong makes every delta exceed the mod's 0.25s sanity clamp, which
    // freezes all animation and kills each rain band on the frame it spawns.
    return static_cast<double>(tm->GetTime()) * 0.001;
}

const ImGuiViewport* GetMainViewport() {
    static ImGuiViewport vp;
    float w = 1600.0f, h = 1200.0f;
    ViewportSize(w, h);
    vp.Pos = ImVec2(0.0f, 0.0f);
    vp.Size = ImVec2(w, h);
    return &vp;
}

ImGuiIO& GetIO() {
    static ImGuiIO io;
    // Legacy BML has no ImGui and therefore no WantTextInput. What it does have
    // is InputHook::IsBlock(), which BML sets while its command bar owns the
    // keyboard (BMLMod.cpp) - the same "an input field would eat typed
    // characters" signal the BML+ build reads from ImGui.
    const bool blocked = InputHook::IsBlock() ? true : false;
    io.WantTextInput = blocked;
    io.WantCaptureKeyboard = blocked;
    return io;
}

ImDrawList* GetForegroundDrawList() {
    static ImDrawList dl;
    return &dl;
}

ImFont* GetFont() {
    static ImFont f;
    return &f;
}

namespace Compat {

bool Init(IBML* bml, ILogger* logger) {
    g_Bml = bml;
    g_Log = logger;
    g_Inited = (bml != nullptr);

    // Deliberately NOT calling BGui::Gui::InitMaterials() here. It re-resolves
    // BML's own button materials (M_Button_Up, M_EntryBG, ...) from the loaded
    // content, and at OnLoad time that content is not there yet, so it would
    // overwrite them with nulls just before BML builds its menus - BML would
    // then render buttonless, invisible menus. BGui::Text picks its font from a
    // file-static that BML fills in later during start-up, and our text elements
    // are created lazily on the first frame, long after that, so we simply
    // inherit whatever BML chose.

    // The render context is already live by the time a mod's OnLoad runs, so
    // seed the cached viewport here: the mod logs its size during OnLoad, before
    // the first BeginFrame would otherwise refresh it.
    RefreshViewport();

    if (g_Log != nullptr) {
        g_Log->Info("[RainingKeys/rk2d] backend ready (Virtools 2D, no ImGui); zBase=%d viewport=%.0fx%.0f",
                    Z_BASE, g_VpW, g_VpH);
    }
    return g_Inited;
}

void BeginFrame() {
    // Hide the previous frame's elements only when that frame never reached
    // EndFrame() - i.e. the mod was gated off and OnProcess returned before
    // DrawOverlay. In the normal case the elements are simply repositioned and
    // reused, which avoids two Show() calls per element per frame.
    if (!g_FrameCompleted) {
        for (size_t i = 0; i < g_Quads.size(); ++i) HideQuad(g_Quads[i]);
        for (size_t i = 0; i < g_TexQuads.size(); ++i) HideTexQuad(g_TexQuads[i]);
        for (size_t i = 0; i < g_Texts.size(); ++i) HideText(g_Texts[i]);
    }
    g_FrameCompleted = false;

    g_Cmds.clear();
    g_QuadNext = 0;
    g_TexQuadNext = 0;
    g_TextNext = 0;

    RefreshViewport();

    if (!g_Probed) {
        g_Probed = true;
        ProbeHotSpot();
        ProbeTextHotSpot();
    }
}

void EndFrame() {
    if (!g_Inited) return;

    int z = Z_BASE;
    for (size_t i = 0; i < g_Cmds.size(); ++i) {
        const Cmd& c = g_Cmds[i];
        switch (c.kind) {
            case Cmd::FILL:
                RoundedFillRect(c.x0, c.y0, c.x1, c.y1, c.c0, c.rounding, z);
                break;
            case Cmd::OUTLINE:
                RoundedOutlineRect(c.x0, c.y0, c.x1, c.y1, c.c0, c.rounding, c.thickness, z);
                break;
            case Cmd::GRADIENT:
                GradientRect(c.x0, c.y0, c.x1, c.y1, c.c0, c.c1, c.c2, c.c3, z);
                break;
            case Cmd::TEXT:
                DrawTextElem(c.fontSize, c.x0, c.y0, c.c0, c.text, z);
                break;
        }
        ++z;
    }

    // Elements the frame did not need go away; the ones it did use stay as they
    // are until the next frame repositions them.
    for (size_t i = g_QuadNext; i < g_Quads.size(); ++i) HideQuad(g_Quads[i]);
    for (size_t i = g_TexQuadNext; i < g_TexQuads.size(); ++i) HideTexQuad(g_TexQuads[i]);
    for (size_t i = g_TextNext; i < g_Texts.size(); ++i) HideText(g_Texts[i]);

    g_LastCommands = static_cast<unsigned>(g_Cmds.size());
    g_Cmds.clear();
    g_FrameCompleted = true;
}

unsigned CommandsLastFrame() { return g_LastCommands; }

unsigned ElementsInUse() {
    return static_cast<unsigned>(g_QuadNext + g_TexQuadNext + g_TextNext);
}

unsigned TexturesCached() { return static_cast<unsigned>(g_TexCache.size()); }

} // namespace Compat
} // namespace ImGui
