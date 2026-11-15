cmake_minimum_required(VERSION 3.31)
project(IsolatedNativeQuestionTests LANGUAGES CXX)
include(CTest)
find_package(iiSDKCore 0.20 CONFIG REQUIRED COMPONENTS Runtime)
get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiMultimodalAgent/Export.h"
    CONTENT "#pragma once\n#include \"${source_root}/src/API/Export.h\"\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiMultimodalAgent/agent/NativeQuestionViewModel.h"
    CONTENT "#pragma once\n#include \"${source_root}/src/Agent/Collaboration/NativeQuestionViewModel.h\"\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiMultimodalAgent/agent/NativeQuestionPresentationC.h"
    CONTENT "#pragma once\n#include \"${source_root}/src/Agent/Collaboration/NativeQuestionPresentationC.h\"\n")
add_library(QuestionPresentationIsolated SHARED "${source_root}/src/Agent/Collaboration/NativeQuestionViewModel.cpp"
    "${source_root}/src/Agent/Collaboration/NativeQuestionPresentationC.cpp")
target_compile_features(QuestionPresentationIsolated PUBLIC cxx_std_23)
target_compile_definitions(QuestionPresentationIsolated PRIVATE IIMULTIMODALAGENT_BUILDING_LIBRARY)
target_include_directories(QuestionPresentationIsolated PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/include")
target_link_libraries(QuestionPresentationIsolated PUBLIC iiSDKCore::Runtime)
add_executable(IsolatedNativeQuestionModel "${source_root}/tests/native_question_view_model.cpp")
target_link_libraries(IsolatedNativeQuestionModel PRIVATE QuestionPresentationIsolated)
set_source_files_properties("${source_root}/tests/question-c-consumer/main.c" PROPERTIES LANGUAGE CXX)
add_executable(IsolatedNativeQuestionCAbi "${source_root}/tests/question-c-consumer/main.c")
target_link_libraries(IsolatedNativeQuestionCAbi PRIVATE QuestionPresentationIsolated)
add_test(NAME Question.IsolatedModel COMMAND IsolatedNativeQuestionModel)
add_test(NAME Question.IsolatedCAbi COMMAND IsolatedNativeQuestionCAbi)
set_tests_properties(Question.IsolatedModel Question.IsolatedCAbi PROPERTIES TIMEOUT 30)
if(APPLE)
    set_tests_properties(Question.IsolatedModel Question.IsolatedCAbi PROPERTIES ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;DYLD_FALLBACK_FRAMEWORK_PATH=unset:")
endif()
