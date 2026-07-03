cmake_minimum_required(VERSION 3.20)
project(TES4MP VERSION 1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output a .dll
add_library(TES4MP SHARED
    src/main.cpp
    src/network.cpp
    src/scriptfuncs.cpp
    src/config.cpp
    src/game_hooks.cpp
    src/ui.cpp
    src/pos_sync.cpp
    src/appearance.cpp
    src/d3d_hook.cpp
    src/ghost_system.cpp
    src/npc_sync.cpp
    src/equip_sync.cpp
    src/quest_sync.cpp
    src/npc_spawn_sync.cpp
)

target_include_directories(TES4MP PRIVATE
    include
    src
)

target_compile_definitions(TES4MP PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601   # Windows 7 minimum (Wine compatible)
)

target_compile_options(TES4MP PRIVATE
    -Wall -Wextra -O2
    # Suppress warnings from OBSE headers we don't control
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

target_link_libraries(TES4MP PRIVATE
    ws2_32    # Winsock
    shell32   # SHGetFolderPath
    advapi32  # GetUserNameA
    d3d9      # D3D9 Present hook
    -static-libgcc
    -static-libstdc++
    # Statically link winpthread and the UCRT so Wine doesn't need external DLLs
    -Wl,-Bstatic,--whole-archive -lpthread -Wl,--no-whole-archive
    -lucrt
)

# Strip the DLL in release to reduce size
set_target_properties(TES4MP PROPERTIES
    PREFIX ""           # No "lib" prefix
    SUFFIX ".dll"
    LINK_FLAGS "-Wl,--enable-stdcall-fixup"
)

# Post-build: copy to Oblivion's OBSE plugin directory under the Wine prefix
set(OBLIVION_DATA "$ENV{HOME}/.local/share/Steam/steamapps/common/Oblivion/Data")
set(OBSE_PLUGIN_DIR "${OBLIVION_DATA}/OBSE/Plugins")

add_custom_command(TARGET TES4MP POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OBSE_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:TES4MP>" "${OBSE_PLUGIN_DIR}/TES4MP.dll"
    # Install default INI only if one doesn't already exist (preserve user edits)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/TES4MP.ini"
        "${OBSE_PLUGIN_DIR}/TES4MP.ini"
    COMMENT "Installed TES4MP.dll + TES4MP.ini -> Oblivion Data"
)
