cmake_minimum_required(VERSION 3.16)
project(linx_host LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT EXISTS "${AI_PASSPORT_DIR}/components/bsp/src/bsp_display_rounding.c")
    message(FATAL_ERROR "缺少已校验的上游依赖，请通过 tools/validate_linx_host.sh 构建")
endif()
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO_ROOT}/managed_components/lvgl__lvgl" lvgl)
add_library(linx_host_app STATIC
    "${REPO_ROOT}/main/linx_ui.c"
    "${REPO_ROOT}/main/linx_demo.c"
    "${REPO_ROOT}/main/linx_content.c"
    "${REPO_ROOT}/main/linx_navigation.c"
    "${REPO_ROOT}/main/linx_reader.c"
    "${REPO_ROOT}/main/linx_input.c"
    "${AI_PASSPORT_DIR}/components/bsp/src/bsp_display_rounding.c"
    "${REPO_ROOT}/assets/fonts/linx_font_12.c"
    "${REPO_ROOT}/assets/fonts/linx_font_14.c"
    "${REPO_ROOT}/assets/fonts/linx_font_16.c"
    "${REPO_ROOT}/assets/fonts/linx_font_20.c"
    "${REPO_ROOT}/assets/fonts/linx_font_16_bold.c"
    "${REPO_ROOT}/assets/fonts/linx_font_20_bold.c"
    "${REPO_ROOT}/assets/fonts/linx_font_24_bold.c"
    "${REPO_ROOT}/assets/fonts/linx_font_28_bold.c")
target_include_directories(linx_host_app PUBLIC "${REPO_ROOT}/main" "${AI_PASSPORT_DIR}/components/bsp/src")
target_compile_options(linx_host_app PRIVATE -Wall -Wextra -Werror)
target_link_libraries(linx_host_app PUBLIC lvgl m)

# Only this copy of the real generated image getter receives fault injection.
add_library(linx_host_missing_image_asset OBJECT "${REPO_ROOT}/assets/images/linx_image_data.c")
target_include_directories(linx_host_missing_image_asset PRIVATE "${REPO_ROOT}/main")
target_compile_definitions(linx_host_missing_image_asset PRIVATE LINX_TEST_MISSING_IMAGE=1)
target_link_libraries(linx_host_missing_image_asset PRIVATE lvgl)

add_executable(linx_host renderer.c "${REPO_ROOT}/assets/images/linx_image_data.c")
add_executable(linx_host_missing_image renderer.c $<TARGET_OBJECTS:linx_host_missing_image_asset>)
add_executable(linx_ui_boundaries test_ui_boundaries.c "${REPO_ROOT}/assets/images/linx_image_data.c")
target_compile_options(linx_ui_boundaries PRIVATE -Wall -Wextra -Werror)
target_link_libraries(linx_ui_boundaries PRIVATE linx_host_app)
foreach(target linx_host linx_host_missing_image)
    target_compile_definitions(${target} PRIVATE LINX_REPO_ROOT="${REPO_ROOT}")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(${target} PRIVATE linx_host_app)
endforeach()

option(LINX_ENABLE_COVERAGE "生成自有 C 应用的 LLVM 覆盖率" OFF)
if(LINX_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "覆盖率构建需要 Clang 和同版本 LLVM 工具")
    endif()
    target_compile_options(linx_host_app PRIVATE -O0 -g -fprofile-instr-generate -fcoverage-mapping)
    target_compile_options(linx_ui_boundaries PRIVATE -O0 -g -fprofile-instr-generate -fcoverage-mapping)
    # 私有单测包含同一 UI 源码，统一静态函数的 profile 名，合并互补分支而非重复统计。
    target_compile_options(linx_ui_boundaries PRIVATE "SHELL:-Xclang -main-file-name -Xclang linx_ui.c")
    foreach(target linx_host linx_host_missing_image linx_ui_boundaries)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    endforeach()
endif()
