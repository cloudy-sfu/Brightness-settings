cmake_minimum_required(VERSION 3.20)
project(Brightness CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ensure Windows 10/11 APIs are visible across all toolchains
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

# Ref: https://github.com/ocornut/imgui
include(FetchContent)
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.8
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
endif()

add_executable(Brightness WIN32 main.cpp)

target_link_libraries(Brightness PRIVATE
        imgui
        dxva2
        d3d11
        d3dcompiler
        dxgi
        gdi32
        dwmapi
        setupapi
        hid
        advapi32
        shell32
)

# Strip binary and suppress console window under MinGW
# Strip binary and suppress console window under MinGW
if(MINGW)
    # Statically link C/C++ runtimes, but leave winpthread dynamic to avoid
    # the __intrinsic_setjmpex toolchain bug on MSVCRT.
    target_link_options(Brightness PRIVATE -mwindows -s -static-libgcc -static-libstdc++)

    # Find where your MinGW compiler stores its DLLs
    get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    # Automatically bundle libwinpthread-1.dll next to the output executable
    add_custom_command(TARGET Brightness POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_BIN_DIR}/libwinpthread-1.dll"
            $<TARGET_FILE_DIR:Brightness>
            COMMENT "Copying libwinpthread-1.dll to output directory..."
    )
endif()