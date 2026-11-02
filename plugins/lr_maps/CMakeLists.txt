# lr_maps — Plugify C++ port of the Pisex LR module "[LR] ExStats Maps"
# (the original LR_ExStatsMaps module, Metamod plugin lr_maps).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The six event
# hooks, the map gate/lifecycle and the client/binding checks live in src/plugin.cpp; the per-map
# counters, the event counters and the `_maps` SQL are the engine-free src/maps_core.*.
#
#   lr_maps_core       engine-free core (the per-map counters, the event counters, the `_maps` SQL;
#                      also used by tests/maps_tests.cpp)
#   plugify-lr_maps    the plugin shared library (entry "bin/lr_maps")

set(LR_MAPS_VERSION "1.1.0")

# --- core ---------------------------------------------------------------------
add_library(lr_maps_core STATIC src/maps_core.cpp)
target_include_directories(lr_maps_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# plgdb is the database layer and the only dependency of the core: the counters and the SQL
# builders are engine-free, so the tests link this target alone.
target_link_libraries(lr_maps_core PUBLIC plgdb)
target_compile_options(lr_maps_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_maps_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin. In the monorepo
# plugins/levels_ranks builds them as the object library `lr_bindings` (it sorts before this
# directory); for an exported repository (no sibling CMakeLists.txt) this directory compiles its
# own copy under a unique target name, so two standalone plugins in one build tree cannot define
# `lr_bindings` twice.
if(TARGET lr_bindings)
    set(LR_MAPS_LR_BINDING_TARGET lr_bindings)
elseif(EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings_lr_maps OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings_lr_maps SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings_lr_maps PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(LR_MAPS_LR_BINDING_TARGET lr_bindings_lr_maps)
else()
    message(FATAL_ERROR "lr_maps needs the generated levels_ranks consumer bindings (plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp)")
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_maps SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:${LR_MAPS_LR_BINDING_TARGET}>
)
target_include_directories(plugify-lr_maps PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_maps SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_maps PRIVATE
        plugify_sdk
        lr_maps_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-lr_maps PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-lr_maps PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_maps"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-lr_maps PROPERTIES OUTPUT_NAME lr_maps)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-lr_maps PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/lr_maps.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lr_maps.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests (the event counters and the SQL against SQLite and the test MySQL of
# ./test.sh); build/tests so ../test.sh runs them.
add_executable(lr_maps_tests tests/maps_tests.cpp)
target_link_libraries(lr_maps_tests PRIVATE lr_maps_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_maps_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_maps_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(lr_maps_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Engine test of the real map-statistics path: src/plugin.cpp is compiled into the test together
# with the generated binding objects, whose __s2sdk_*/__levels_ranks_* globals are null until the
# test assigns fakes. It drives the plugin through its own Plugify_* entry points, fires the fake
# events (player-typed fields answer a different value through GetEventInt), runs the map
# change/disconnect/slot-reuse paths against a temporary SQLite file and proves that OnPluginEnd
# removes every registration. Nothing here links pisexcompat.
add_executable(lr_maps_event_tests
        tests/event_tests.cpp
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:${LR_MAPS_LR_BINDING_TARGET}>
)
target_include_directories(lr_maps_event_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources (src/plugin.cpp is one of them).
target_include_directories(lr_maps_event_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(lr_maps_event_tests PRIVATE
        plugify_sdk
        lr_maps_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(lr_maps_event_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_maps_event_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_maps"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(lr_maps_event_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
