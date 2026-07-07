cmake_minimum_required(VERSION 3.10)
project(MiniTextSearch VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ===================== 源文件 =====================
set(SOURCES
    main_proj/main.c
    dll_src/text_io/text_io.c
    dll_src/index_build/index_build.c
    dll_src/query_handle/query_handle.c
    dll_src/rank_score/rank_score.c
    dll_src/result_out/result_out.c
)

# ===================== 可执行文件 =====================
add_executable(mini_search ${SOURCES})

# ===================== 头文件搜索路径 =====================
target_include_directories(mini_search PRIVATE
    ${CMAKE_SOURCE_DIR}/public
    ${CMAKE_SOURCE_DIR}
)

# ===================== 编译选项 =====================
if(MSVC)
    target_compile_options(mini_search PRIVATE /W4)
    target_compile_definitions(mini_search PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mini_search PRIVATE -Wall -Wextra -g)
endif()

# ===================== 平台特定链接 =====================
if(UNIX)
    # Unix 需要显式链接数学库
    target_link_libraries(mini_search PRIVATE m)
endif()
