cmake_minimum_required(VERSION 3.14)

project(RainingKeys VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 依赖查找
#   BML          : BML+ 官方 SDK（含 BMLPlus.lib 导入库与头文件）
#   VirtoolsSDK  : Virtools 2.1 SDK（CK2.lib / VxMath.lib），由 BML 间接依赖
# 通过 -DCMAKE_PREFIX_PATH=... 与 -DVIRTOOLS_SDK_PATH=... 指定
# ---------------------------------------------------------------------------
list(INSERT CMAKE_MODULE_PATH 0 "${VIRTOOLS_SDK_PATH}/CMake")
find_package(BML REQUIRED)

add_library(RainingKeys SHARED
    src/RainingKeys.cpp
)

# 版本精确匹配的 Dear ImGui 头文件。
# 宿主 BMLPlus 0.3.14 内嵌的是 Dear ImGui 1.92.9b，而官方 0.3.13 SDK 附带的是 1.92.8。
# 用错版本的头文件去操作宿主的 ImGui 上下文会出问题（这正是 P0 v1 画不出东西的原因），
# 因此这里把匹配版本的头放在 SDK include 之前。
set(IMGUI_OVERRIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/imgui-override")
if (EXISTS "${IMGUI_OVERRIDE_DIR}")
    target_include_directories(RainingKeys BEFORE PRIVATE "${IMGUI_OVERRIDE_DIR}")
    message(STATUS "[RainingKeys] ImGui 头文件覆盖目录: ${IMGUI_OVERRIDE_DIR}")
endif ()

target_include_directories(RainingKeys PRIVATE ${BML_INCLUDE_DIRS})

# MSVC 上关闭字符串字面量严格性（Virtools SDK 头文件的既有写法要求）
target_compile_options(RainingKeys PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:strictStrings->
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(RainingKeys PRIVATE BML CK2 VxMath)

set_target_properties(RainingKeys PROPERTIES
    OUTPUT_NAME "RainingKeys"
    SUFFIX ".bmodp"
    PREFIX ""
)
