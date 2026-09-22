cmake_minimum_required(VERSION 3.24)
project(ghub-lite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT WIN32)
    message(FATAL_ERROR "ghub-lite ist reines Win32 (HID++ ueber den Windows-HID-Stack).")
endif()

if(MSVC)
    # Statische CRT: die Exe soll ohne installiertes VC++-Redistributable laufen. Nebenbei
    # spart das zwei geladene DLLs und damit Working Set.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

    # /GR-  kein RTTI -- im Projekt gibt es weder dynamic_cast noch typeid.
    # /Gy   jede Funktion in ein eigenes COMDAT; erst damit kann der Linker unten
    #       ungenutzte entfernen (/OPT:REF) und identische zusammenlegen (/OPT:ICF).
    # /Zc:threadSafeInit-  spart die Sperren um funktionslokale Statics; wir haben keine.
    add_compile_options(/GR- /Gy /Zc:threadSafeInit-)
    add_link_options($<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                     $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)

    # Bewusst NICHT gesetzt:
    #   /GS-     spart wenige KB, aber wir werten Bytes aus, die vom Geraet kommen.
    #   /EHs-c-  die STL wirft (bad_alloc, std::thread) -- auch ohne eigenes throw.
    #   /Os      waere eine Messung wert, ist aber ohne Messgrund keine Voreinstellung.
endif()

# Modulübergreifende Optimierung (/GL + /LTCG) nur im Release: sie kostet Buildzeit und
# bringt beim Entwickeln nichts.
include(CheckIPOSupported)
check_ipo_supported(RESULT gl_ipo_ok OUTPUT gl_ipo_reason)
if(gl_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "ghub-lite: ohne LTCG -- ${gl_ipo_reason}")
endif()

function(gl_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# ghub_hidpp: HID-Enumeration + HID++-2.0-Transport + Features.
# Keine UI-Abhaengigkeit, damit die Konsolen-Probe dieselbe Codebasis benutzt.
# ---------------------------------------------------------------------------
add_library(ghub_hidpp STATIC
    src/hidpp/hid_enum.cpp
    src/hidpp/hidpp_device.cpp
    src/hidpp/hidpp_root.cpp
    src/hidpp/feat_info.cpp
    src/hidpp/feat_dpi.cpp
    src/hidpp/feat_rate.cpp
    src/hidpp/feat_gkeys.cpp
    src/hidpp/device_manager.cpp
)
target_include_directories(ghub_hidpp PUBLIC src)
target_link_libraries(ghub_hidpp PUBLIC setupapi hid cfgmgr32)
gl_set_warnings(ghub_hidpp)

# ---------------------------------------------------------------------------
# hidpp_dump: Konsolen-Probe. Erzeugt die Referenzdaten in docs/devlog/.
# ---------------------------------------------------------------------------
add_executable(hidpp_dump tools/hidpp_dump.cpp)
target_link_libraries(hidpp_dump PRIVATE ghub_hidpp)
gl_set_warnings(hidpp_dump)



# ---------------------------------------------------------------------------
# ghub-lite: die GUI. Einzelbinary -- statische CRT, keine externen Abhaengigkeiten.
# ---------------------------------------------------------------------------
add_executable(ghub-lite WIN32
    src/app/main.cpp
    src/app/settings.cpp
    src/app/keystroke.cpp
    src/ui/theme.cpp
    src/ui/main_window.cpp
    src/ui/dpi_panel.cpp
    src/ui/rate_panel.cpp
    src/ui/gkey_panel.cpp
    src/ui/tray.cpp
    res/app.rc
)
target_link_libraries(ghub-lite PRIVATE
    ghub_hidpp comctl32 dwmapi uxtheme shell32 advapi32 gdi32 user32)
gl_set_warnings(ghub-lite)

# Das Manifest kommt ueber res/app.rc; den vom Linker erzeugten unterdruecken,
# sonst gibt es zwei RT_MANIFEST-Ressourcen und der Build bricht ab.
if(MSVC)
    set_target_properties(ghub-lite PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

# ---------------------------------------------------------------------------
# ghub-lite-setup: Installer pro Benutzer. Traegt ghub-lite.exe als Ressource in sich,
# bleibt also selbst ein einzelnes Binary zum Weitergeben.
# ---------------------------------------------------------------------------
# Die Exe erst an einen festen Ort kopieren: der Pfad muss beim Konfigurieren feststehen,
# weil er in die erzeugte .rc geschrieben wird, und $<TARGET_FILE> haengt an der
# Konfiguration. Ein Debug-Build ueberschreibt die Nutzlast deshalb mit der Debug-Exe.
set(GL_PAYLOAD ${CMAKE_BINARY_DIR}/payload/ghub-lite.exe)
add_custom_command(
    OUTPUT ${GL_PAYLOAD}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/payload
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ghub-lite> ${GL_PAYLOAD}
    DEPENDS ghub-lite
    COMMENT "Nutzlast fuer das Setup: ghub-lite.exe"
    VERBATIM)
configure_file(res/setup_payload.rc.in ${CMAKE_BINARY_DIR}/setup_payload.rc @ONLY)
set_source_files_properties(${GL_PAYLOAD} PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(ghub-lite-setup WIN32
    src/setup/setup.cpp
    src/app/settings.cpp
    res/setup.rc
    ${CMAKE_BINARY_DIR}/setup_payload.rc
    ${GL_PAYLOAD}
)
target_include_directories(ghub-lite-setup PRIVATE src)
target_compile_definitions(ghub-lite-setup PRIVATE GL_VERSION="${PROJECT_VERSION}")
# Neue Exe -> .rc neu uebersetzen. Ohne das haelt der Generator die Ressource fuer aktuell,
# weil sich die .rc selbst nicht geaendert hat.
set_source_files_properties(${CMAKE_BINARY_DIR}/setup_payload.rc PROPERTIES
    OBJECT_DEPENDS ${GL_PAYLOAD})
target_link_libraries(ghub-lite-setup PRIVATE
    comctl32 ole32 uuid shell32 advapi32 user32)
gl_set_warnings(ghub-lite-setup)
if(MSVC)
    set_target_properties(ghub-lite-setup PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()
