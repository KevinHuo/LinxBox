cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "linx-demo-0.2.0")
# 锁定版本或准备逻辑变化时，已有构建目录也必须重新校验依赖。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/upstream.json"
    "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_upstream.py"
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_upstream.py" --print-path
    RESULT_VARIABLE upstream_result
    OUTPUT_VARIABLE AI_PASSPORT_DIR
    ERROR_VARIABLE upstream_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT upstream_result EQUAL 0)
    message(FATAL_ERROR "无法准备固定版本的 AI Passport 依赖：${upstream_error}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/hardware/folotoy/ai-passport/board.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(FoloToy-AI-Passport)
