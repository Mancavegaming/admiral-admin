cmake_minimum_required(VERSION 3.22)
set(TARGET AdmiralsPanelNative)
project(${TARGET})

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/http_server.cpp
    src/ap_config.cpp
    src/ap_app.cpp
    src/ap_status_source.cpp
)
target_link_libraries(${TARGET} PUBLIC version)

target_include_directories(${TARGET} PRIVATE "include")
target_link_libraries(${TARGET} PUBLIC UE4SS)
# Winsock2 for the standalone HTTP server (v0.6+).
target_link_libraries(${TARGET} PUBLIC ws2_32)
