cmake_minimum_required(VERSION 3.13)
include(~/Documents/CodeLibraries/pico-sdk/pico_sdk_init.cmake)
project(my_project)
pico_sdk_init()

# --- Lua 5.4 ---
file(GLOB LUA_SOURCES
    /home/nathancharlesjones/Documents/CodeLibraries/lua-5.5.0/src/*.c
)

# Exclude the standalone interpreter and compiler — not needed on-device
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*(lua|luac)\\.c$")

add_library(lua55 STATIC ${LUA_SOURCES})
target_include_directories(lua55 PUBLIC ~/Documents/CodeLibraries/lua-5.5.0/src)
target_compile_definitions(lua55 PRIVATE
    LUA_32BITS          # use 32-bit integers/floats to match RP2040
)

# --- embedded_cli ---
add_library(embedded_cli STATIC
    ~/Documents/CodeLibraries/embeddedcli/embedded_cli.c
)
target_include_directories(embedded_cli PUBLIC
    ~/Documents/CodeLibraries/embeddedcli
)

# --- Application ---
add_executable(rp2040_and_HAL
    main.c
)

target_sources(rp2040_and_HAL PRIVATE
    lua_hal_RP2040.c
)

target_include_directories(rp2040_and_HAL PUBLIC
    .
)

target_link_libraries(rp2040_and_HAL
    pico_stdlib
    lua55
    embedded_cli
)
pico_enable_stdio_usb(rp2040_and_HAL 1)
pico_enable_stdio_uart(rp2040_and_HAL 0)
pico_add_extra_outputs(rp2040_and_HAL)