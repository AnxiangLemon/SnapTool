cmake_minimum_required(VERSION 3.16)
project(SnapTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(SnapTool WIN32 src/main.cpp)

target_compile_definitions(SnapTool PRIVATE UNICODE _UNICODE)
target_link_libraries(SnapTool PRIVATE
    user32
    gdi32
    gdiplus
    shell32
    comdlg32
    dwmapi
    advapi32
)

if(MINGW)
    target_compile_options(SnapTool PRIVATE -Os -Wall -Wextra)
    target_link_options(SnapTool PRIVATE -s -municode -static -static-libgcc -static-libstdc++)
elseif(MSVC)
    target_compile_options(SnapTool PRIVATE /W4 /O2 /utf-8)
    target_link_options(SnapTool PRIVATE /ENTRY:wWinMainCRTStartup)
endif()
