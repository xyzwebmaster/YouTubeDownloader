cmake_minimum_required(VERSION 3.16)
project(YouTubeDownloader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(YouTubeDownloader WIN32
    main.cpp
    app.cpp
    download.cpp
    process.cpp
    util.cpp
    http.cpp
    json.cpp
    settings.cpp
    oauth.cpp
    tiktok_dialog.cpp
)

target_compile_definitions(YouTubeDownloader PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0601
)

target_link_libraries(YouTubeDownloader PRIVATE
    comctl32
    comdlg32
    shell32
    ole32
    uuid
    winhttp
    crypt32
    bcrypt
    ws2_32
)

if(MINGW)
    target_link_options(YouTubeDownloader PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(MSVC)
    target_compile_options(YouTubeDownloader PRIVATE /utf-8 /W3)
endif()
