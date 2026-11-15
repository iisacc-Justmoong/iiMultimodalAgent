cmake_minimum_required(VERSION 3.31)
project(NativeQuestionLvuiConsumer LANGUAGES CXX)
include(CTest)
find_package(iiSDKCore 0.20 CONFIG REQUIRED COMPONENTS Runtime)
find_package(LVUI 0.1 CONFIG REQUIRED COMPONENTS Native)
find_package(iiGUI 0.5 CONFIG REQUIRED)
set(IIGUI_TEST_HEADERS "" CACHE PATH "iiGUI build-tree include directory containing WidgetState.h")
if(NOT EXISTS "${IIGUI_TEST_HEADERS}/iiGUI/WidgetState.h")
    message(FATAL_ERROR "IIGUI_TEST_HEADERS must expose the iiGUI headless test host")
endif()
lvui_generate(NAME UserQuestionsSheet SOURCE ../../src/Views/UserQuestionsSheet.lvui
    FRAMEWORK iiGUI INCLUDES NativeQuestionViewModel.h OUTPUT_VARIABLE generated)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiMultimodalAgent/Export.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/../../src/API/Export.h\"\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiMultimodalAgent/agent/NativeQuestionPresentationC.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/../../src/Agent/Collaboration/NativeQuestionPresentationC.h\"\n")
add_executable(NativeQuestionLvuiConsumer consumer.cpp mount.cpp
    ../../src/Agent/Collaboration/NativeQuestionViewModel.cpp
    ../../src/Agent/Collaboration/NativeQuestionPresentationC.cpp "${generated}")
target_compile_features(NativeQuestionLvuiConsumer PRIVATE cxx_std_23)
target_compile_definitions(NativeQuestionLvuiConsumer PRIVATE IIMULTIMODALAGENT_BUILDING_LIBRARY)
target_include_directories(NativeQuestionLvuiConsumer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Agent/Collaboration"
    "${IIGUI_TEST_HEADERS}")
target_link_libraries(NativeQuestionLvuiConsumer PRIVATE LVUI::Native iiGUI::iiGUI iiSDKCore::Runtime)
add_test(NAME NativeQuestion.LvuiConsumer COMMAND NativeQuestionLvuiConsumer)
if(APPLE)
    set_tests_properties(NativeQuestion.LvuiConsumer PROPERTIES ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;DYLD_FALLBACK_FRAMEWORK_PATH=unset:")
endif()
