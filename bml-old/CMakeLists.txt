# ===========================================================================
# RainingKeys - legacy BML (.bmod) target
#
# Builds  ../src/bml-old/RainingKeys.cpp  against the ORIGINAL Ballance Mod
# Loader SDK (not BML+). The BML+ target is ../CMakeLists.txt and produces
# RainingKeys.bmodp; this one produces RainingKeys.bmod.
#
# Why a separate target instead of one binary for both loaders:
#   * the two loaders have different IMod vtables (0.3.34 added
#     OnPhysicalize / OnUnphysicalize to IMod), so a single binary cannot serve
#     both without reordering the virtuals;
#   * legacy BML exports no ImGui symbol at all, so the drawing layer differs.
#
# SDK: BML dev 0.3.40 - deliberately the OLDEST SDK whose headers and exports we
#      can build against, not the newest. The BML version a mod declares is the
#      one baked into the SDK's BuildVer.h, and the loader skips any mod that
#      asks for more than the running loader is, so declaring 0.3.40 makes one
#      .bmod load on every loader from 0.3.40 up. The 0.3.40 and 0.3.43 headers
#      are byte-identical after newline normalisation, so this costs nothing:
#      no virtual table or structure layout can drift at run time. (Oldest
#      verified-complete: 0.3.40. 0.3.33 looks compatible but is untested;
#      0.3.24 cannot work - it does not export CKTexture::SetDesiredVideoFormat.
#      See CHANGELOG.md section 13.10.)
#
# KNOWN SDK DEFECT (worked around in the source, not here): the import library
# that ships in BML-dev-0343/lib/Release/BML.lib is stale. Its headers declare
# IMod::OnPhysicalize / OnUnphysicalize (added to the loader in 0.3.34) but the
# library exports no such symbols - BML-dev-0340's copy does, and so does the
# SDK this target builds against now, so the no-op overrides in
# src/bml-old/RainingKeys.cpp are redundant here. They are kept on purpose: they
# cost nothing, they keep one source tree buildable against either SDK, and they
# document the trap. Verify a build with:
#   dumpbin /imports Release\RainingKeys.bmod   vs   dumpbin /exports BML.dll
#
# Configure with:
#   cmake -S bml-old -B <builddir> -G "Visual Studio 17 2022" -A Win32
# ===========================================================================
cmake_minimum_required(VERSION 3.14)

project(RainingKeysBmlOld LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED BML_OLD_SDK_PATH)
    set(BML_OLD_SDK_PATH "E:/ViewDistanceEditor/SDK/BML-dev-0340" CACHE PATH "Legacy BML SDK root")
endif()

if(NOT EXISTS "${BML_OLD_SDK_PATH}/include/BML/BMLAll.h")
    message(FATAL_ERROR "[RainingKeys] Legacy BML SDK not found at ${BML_OLD_SDK_PATH}. "
                        "Pass -DBML_OLD_SDK_PATH=<path to a BML dev SDK, e.g. BML-dev-0340>.")
endif()

# rk_imgui_compat.cpp is the Virtools 2D backend that stands in for Dear ImGui
# on this host; src/bml-old/RainingKeys.cpp includes the matching facade header
# and is otherwise a near-verbatim copy of the BML+ source.
add_library(RainingKeysBmlOld SHARED
    ../src/bml-old/RainingKeys.cpp
    ../src/bml-old/rk_imgui_compat.cpp
)

# No ImGui override here: this host has no ImGui, and its own headers must win,
# so the only include path is the legacy SDK.
target_include_directories(RainingKeysBmlOld PRIVATE "${BML_OLD_SDK_PATH}/include")

target_link_libraries(RainingKeysBmlOld PRIVATE "${BML_OLD_SDK_PATH}/lib/Release/BML.lib")

target_compile_definitions(RainingKeysBmlOld PRIVATE _CRT_SECURE_NO_WARNINGS)

# /Zc:strictStrings- : the legacy SDK headers assign string literals to char*
target_compile_options(RainingKeysBmlOld PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:strictStrings->
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

set_target_properties(RainingKeysBmlOld PROPERTIES
    OUTPUT_NAME "RainingKeys"
    SUFFIX ".bmod"
    PREFIX ""
)
