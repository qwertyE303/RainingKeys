#pragma once
// ===========================================================================
// rk_imgui_compat.hpp - the legacy-BML stand-in for imgui.h
// ---------------------------------------------------------------------------
// This header is *only* for the legacy (original Gamepiaynmo) Ballance Mod
// Loader target, which has no Dear ImGui at all: BML 0.3.43 exports 2433
// symbols and not one of them is an ImGui symbol (verified with dumpbin).
//
// It reproduces the exact subset of the ImGui API that RainingKeys uses, on
// top of Virtools 2D entities, so that ../bml-old/RainingKeys.cpp can stay a
// near-verbatim copy of the BML+ source (../RainingKeys.cpp). The whole point
// is that the two files keep diffing cleanly against each other; everything
// host specific is hidden behind this facade.
//
// Only these names are part of the contract - the list is closed:
//
//   ImGui::GetTime()                ImGui::GetMainViewport()
//   ImGui::GetIO()                  ImGui::GetForegroundDrawList()
//   ImGui::GetFont()
//
//   ImDrawList::AddRectFilled       ImDrawList::AddRect
//   ImDrawList::AddRectFilledMultiColor
//   ImDrawList::AddText             ImFont::CalcTextSizeA
//
//   ImVec2  ImU32  IM_COL32  ImGuiViewport  ImGuiIO  ImFont  ImDrawList
//   IMGUI_VERSION
//
// Two additions the BML+ code does not need, because a retained-mode backend
// has no per-primitive submission:
//
//   ImGui::Compat::Init(m_bml, GetLogger());   // once, in OnLoad
//   ImGui::Compat::BeginFrame();               // top of DrawOverlay
//   ImGui::Compat::EndFrame();                 // bottom of DrawOverlay
//
// See rk_imgui_compat.cpp for how each primitive is realised, and for the
// CalcTextSizeA convention that keeps every text call site centred without a
// text-metric API.
// ===========================================================================

// Guard: mixing this facade with the real Dear ImGui headers (i.e. pulling it
// into the BML+ target by accident) must fail loudly at compile time, not at
// link time or - worse - silently at run time.
#if defined(IMGUI_VERSION) || defined(IMGUI_H) || defined(BML_PLUS) || \
    defined(BMLPLUS_VERSION) || defined(BML_PLUS_VERSION)
#  error "rk_imgui_compat.hpp is the legacy-BML replacement for imgui.h and must never be included by the BML+ target."
#endif

#include <BML/BMLAll.h>

// ---------------------------------------------------------------------------
// Version string, so the shared log line ("ImGui headers %s") still compiles
// and tells the truth about which backend is running.
// ---------------------------------------------------------------------------
#define IMGUI_VERSION "rk-compat/1 (Virtools 2D backend, no ImGui)"

// ---------------------------------------------------------------------------
// Entry-point macro. BML+ gets MOD_EXPORT from <BML/Defines.h>; the legacy SDK
// has no such header, so it is supplied here. Keeping the same name means the
// BMLEntry declaration reads identically in both source files.
// ---------------------------------------------------------------------------
#ifndef MOD_EXPORT
#  define MOD_EXPORT extern "C" __declspec(dllexport)
#endif

// ===========================================================================
// ImGui value types
// ===========================================================================

struct ImVec2 {
    float x = 0.0f;
    float y = 0.0f;
    ImVec2() = default;
    ImVec2(float ax, float ay) : x(ax), y(ay) {}
};

typedef unsigned int ImU32;

// Same packing as the real ImGui (A<<24 | B<<16 | G<<8 | R), so the shared
// ToU32()/PackColor() helpers in RainingKeys.cpp need no changes. The backend
// converts to Virtools' own A<<24 | R<<16 | G<<8 | B layout internally.
#define IM_COL32(R, G, B, A)                                              \
    (((ImU32)(A) << 24) | ((ImU32)(B) << 16) | ((ImU32)(G) << 8) | (ImU32)(R))

struct ImGuiViewport {
    ImVec2 Pos;
    ImVec2 Size;
};

struct ImGuiIO {
    bool WantCaptureKeyboard = false;
    bool WantTextInput = false;
};

// ---------------------------------------------------------------------------
// Font
//
// The legacy host exposes no text-measurement API at all (BGui::Text wraps
// CKSpriteText, which can only *draw* text). Instead of guessing a width, the
// facade returns a zero width and a height equal to the requested size:
//
//   * every call site in RainingKeys.cpp centres text with  x - ts.x * 0.5f,
//     so a zero width turns that into exactly  x  - the centre it wanted - and
//     the backend then centres the sprite on that point with
//     CKSPRITETEXT_HCENTER | CKSPRITETEXT_VCENTER;
//   * ts.y == size keeps the vertical centring arithmetic (y - ts.y * 0.5f)
//     working the same way ImGui's line height did.
//
// So centring is exact without ever needing a measurement.
// ---------------------------------------------------------------------------
struct ImFont {
    ImVec2 CalcTextSizeA(float size, float max_width, float wrap_width,
                         const char* text_begin) const;
};

// ===========================================================================
// Draw list
//
// In ImGui this is an immediate-mode geometry sink. Here it is a per-frame
// command buffer that EndFrame() realises into pooled 2D elements. Because the
// shared code only ever emits primitives in painting order, EndFrame() assigns
// the z-order from the submission index, which preserves that order exactly.
// ===========================================================================
struct ImDrawList {
    void AddRectFilled(const ImVec2& pmin, const ImVec2& pmax, ImU32 col,
                       float rounding = 0.0f, int flags = 0);
    void AddRect(const ImVec2& pmin, const ImVec2& pmax, ImU32 col,
                 float rounding = 0.0f, int flags = 0, float thickness = 1.0f);
    void AddRectFilledMultiColor(const ImVec2& pmin, const ImVec2& pmax,
                                 ImU32 col_upr_left, ImU32 col_upr_right,
                                 ImU32 col_bot_right, ImU32 col_bot_left);
    void AddText(ImFont* font, float font_size, const ImVec2& pos, ImU32 col,
                 const char* text_begin);
};

// ===========================================================================
// Entry points
// ===========================================================================
namespace ImGui {

double GetTime();

const ImGuiViewport* GetMainViewport();

ImGuiIO& GetIO();

ImDrawList* GetForegroundDrawList();

ImFont* GetFont();

namespace Compat {

// Wires the facade to the running host. Call once from the mod's OnLoad.
bool Init(IBML* bml, ILogger* logger);

// Starts a new frame: clears the command buffer.
void BeginFrame();

// Realises the frame's commands into the pooled 2D elements and hides any
// element the frame did not use.
void EndFrame();

// Diagnostics for the mod's own selfcheck log.
unsigned CommandsLastFrame();
unsigned ElementsInUse();
unsigned TexturesCached();

} // namespace Compat
} // namespace ImGui
