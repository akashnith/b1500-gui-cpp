cmake_minimum_required(VERSION 3.21)
project(B1500ANeuromorphicGUI VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(B1500A_ENABLE_VISA "Enable the native Windows VISA backend" OFF)
set(B1500A_WGFMU_LIBRARY "" CACHE FILEPATH "Optional Keysight WGFMU library")

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Charts Test Network)

add_library(b1500a_core
    src/core/measurement.cpp
    src/core/patternbuilder.cpp
    src/core/csvwriter.cpp
    src/core/ferroanalysis.cpp
    src/instruments/b1500demo.cpp
    src/instruments/b1500visa.cpp
    src/instruments/wgfmuapi.cpp
)
target_include_directories(b1500a_core PUBLIC src)
target_link_libraries(b1500a_core PUBLIC Qt6::Core)
if(WIN32)
    target_link_libraries(b1500a_core PRIVATE Version)
endif()

# The executable's own icon and version block, for Explorer and the taskbar.
# Qt's setWindowIcon covers the window; this covers the file itself.
set(B1500A_WINDOWS_RESOURCE "")
if(WIN32)
    enable_language(RC)
    set(B1500A_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.ico")
    set(B1500A_WINDOWS_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/appicon.rc")
    configure_file(resources/appicon.rc.in "${B1500A_WINDOWS_RESOURCE}" @ONLY)
endif()

add_executable(b1500a_neuro_gui
    src/app/main.cpp
    src/app/mainwindow.cpp
    src/app/mainwindow.h
    src/app/mainwindow_bridge.cpp
    src/app/assistantbridge.cpp
    src/app/assistantbridge.h
    src/app/runworker.cpp
    src/app/runworker.h
    src/app/unitspinbox.cpp
    src/app/unitspinbox.h
    src/ai/claudeclient.cpp
    src/ai/claudeclient.h
    resources/app.qrc
)
target_link_libraries(b1500a_neuro_gui PRIVATE
    b1500a_core
    Qt6::Widgets
    Qt6::Charts
    Qt6::Network
)
if(WIN32)
    # The resource is compiled in a target of its own, with no include
    # directories on it.
    #
    # MinGW's windres hands its -I flags to the C preprocessor unquoted, so one
    # include path containing a space - "VS Code" here - breaks the resource
    # compile. Attaching the script to the GUI target would inherit that
    # target's include paths; a bare object library inherits nothing, and the
    # script includes nothing, so there is nothing to inherit.
    add_library(b1500a_appicon OBJECT "${B1500A_WINDOWS_RESOURCE}")
    set_target_properties(b1500a_appicon PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_sources(b1500a_neuro_gui PRIVATE $<TARGET_OBJECTS:b1500a_appicon>)
endif()
file(COPY presets DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

enable_testing()
# unitspinbox.cpp is compiled into the test as well as the GUI. It parses the
# operator's typed compliance and timing values, where a wrong exponent is a
# factor of a million applied to a real instrument output, so it is tested.
add_executable(b1500a_core_tests tests/test_core.cpp src/app/unitspinbox.cpp)
target_link_libraries(b1500a_core_tests PRIVATE b1500a_core Qt6::Test Qt6::Widgets)
target_include_directories(b1500a_core_tests PRIVATE src/app)
add_library(wgfmu_mock SHARED tests/mock_wgfmu.cpp)
target_compile_definitions(b1500a_core_tests PRIVATE
    WGFMU_MOCK_LIBRARY_PATH="$<TARGET_FILE:wgfmu_mock>"
)
add_dependencies(b1500a_core_tests wgfmu_mock)
add_test(NAME b1500a_core_tests COMMAND b1500a_core_tests)
add_test(NAME b1500a_demo_smoke COMMAND b1500a_neuro_gui --smoke-demo)
set_tests_properties(b1500a_demo_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(B1500A_ENABLE_VISA)
    if(NOT WIN32)
        message(FATAL_ERROR "B1500A_ENABLE_VISA requires a native Windows build with a compatible VISA runtime.")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "B1500A hardware support requires a native x64 build.")
    endif()
    if(NOT B1500A_WGFMU_LIBRARY)
        set(_wgfmu_x64_library "C:/Program Files/Agilent/B1530A/Lib_x64/wgfmu.lib")
        if(EXISTS "${_wgfmu_x64_library}")
            set(B1500A_WGFMU_LIBRARY "${_wgfmu_x64_library}" CACHE FILEPATH
                "Optional Keysight WGFMU library" FORCE)
        else()
            message(FATAL_ERROR
                "Set B1500A_WGFMU_LIBRARY to the installed x64 wgfmu.lib.")
        endif()
    endif()
    if(NOT EXISTS "${B1500A_WGFMU_LIBRARY}")
        message(FATAL_ERROR "WGFMU import library not found: ${B1500A_WGFMU_LIBRARY}")
    endif()
    target_link_libraries(b1500a_core PRIVATE "${B1500A_WGFMU_LIBRARY}")
    target_compile_definitions(b1500a_core PUBLIC B1500A_ENABLE_VISA)
endif()
