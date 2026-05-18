cmake_minimum_required(VERSION 3.16)
project(L2VoiceChat)

set(CMAKE_CXX_STANDARD 17)

add_library(dinput8 SHARED dllmain.cpp)

target_link_libraries(dinput8 PRIVATE ws2_32)

# Gera dinput8.dll diretamente
set_target_properties(dinput8 PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
)

# Para build 32-bit (L2.exe é 32-bit)
# cmake -A Win32 ..