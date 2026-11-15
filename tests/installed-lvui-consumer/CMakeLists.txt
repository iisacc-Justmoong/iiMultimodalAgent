cmake_minimum_required(VERSION 3.31)
project(InstalledNativeQuestionView LANGUAGES CXX)
include(CTest)

find_package(iiMultimodalAgent 0.1 CONFIG REQUIRED)
find_package(LVUI 0.1 CONFIG REQUIRED COMPONENTS Native)
find_package(iiGUI 0.5 CONFIG REQUIRED)
set(IIGUI_WIDGET_STATE_HEADER "" CACHE FILEPATH "iiGUI private headless test host")
if(NOT EXISTS "${IIGUI_WIDGET_STATE_HEADER}")
    message(FATAL_ERROR "IIGUI_WIDGET_STATE_HEADER must identify the headless test host")
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/private/iiGUI/WidgetState.h"
    CONTENT "#pragma once\n#include \"${IIGUI_WIDGET_STATE_HEADER}\"\n")
lvui_generate(NAME UserQuestionsSheet SOURCE "${IIMULTIMODALAGENT_VIEWS_DIR}/UserQuestionsSheet.lvui"
    FRAMEWORK iiGUI INCLUDES iiMultimodalAgent/agent/NativeQuestionViewModel.h OUTPUT_VARIABLE generated)
add_executable(InstalledNativeQuestionView ../lvui-consumer/consumer.cpp ../lvui-consumer/mount.cpp "${generated}")
target_compile_features(InstalledNativeQuestionView PRIVATE cxx_std_23)
target_include_directories(InstalledNativeQuestionView PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/private")
target_link_libraries(InstalledNativeQuestionView PRIVATE iiMultimodalAgent::iiMultimodalAgent LVUI::Native iiGUI::iiGUI)
add_test(NAME NativeQuestion.InstalledLvuiConsumer COMMAND InstalledNativeQuestionView)
if(APPLE)
    set_tests_properties(NativeQuestion.InstalledLvuiConsumer PROPERTIES ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;DYLD_FALLBACK_FRAMEWORK_PATH=unset:")
endif()
