cmake_minimum_required(VERSION 3.24)
project(iiMultimodalAgentConsumer LANGUAGES CXX)
find_package(iiMultimodalAgent 0.1 CONFIG REQUIRED)
if(NOT EXISTS "${IIMULTIMODALAGENT_VIEWS_DIR}/UserQuestionsSheet.lvui" OR
   EXISTS "${IIMULTIMODALAGENT_VIEWS_DIR}/UserQuestionsSheet.qml" OR
   NOT EXISTS "${IIMULTIMODALAGENT_LEGACY_QML_DIR}/UserQuestionsSheet.qml" OR
   NOT IIMULTIMODALAGENT_QML_DIR STREQUAL IIMULTIMODALAGENT_LEGACY_QML_DIR)
    message(FATAL_ERROR "Native and legacy question views were not packaged separately")
endif()
if(NOT TARGET iiMultimodalAgent::NativeImages)
    message(FATAL_ERROR "The installed agent package must expose its native image runtime to bundle deployers")
endif()
enable_testing()
include(../RuntimeBindings.cmake)
foreach(_kind decision_gate decision_engine multimodal multimodal_services session_history memory_dream)
    add_executable(agent_${_kind}_consumer ../${_kind}_tests.cpp)
    target_link_libraries(agent_${_kind}_consumer PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(agent_${_kind}_consumer PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiMultimodalAgent.installed_${_kind} COMMAND agent_${_kind}_consumer)
endforeach()
add_executable(agent_consumer agent.cpp)
target_link_libraries(agent_consumer PRIVATE iiMultimodalAgent::iiMultimodalAgent)
add_test(NAME iiMultimodalAgent.installed_agent COMMAND agent_consumer)
add_executable(agent_question_model_consumer ../native_question_view_model.cpp)
target_compile_features(agent_question_model_consumer PRIVATE cxx_std_23)
target_link_libraries(agent_question_model_consumer PRIVATE iiMultimodalAgent::iiMultimodalAgent)
add_test(NAME iiMultimodalAgent.installed_question_model COMMAND agent_question_model_consumer)
option(IIMULTIMODALAGENT_TEST_BUNDLED_VOICE "Run the installed native speech model" OFF)
if(IIMULTIMODALAGENT_TEST_BUNDLED_VOICE)
    add_executable(speech_consumer ../speech_tool_smoke.cpp)
    target_link_libraries(speech_consumer PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.installed_speech COMMAND speech_consumer)
endif()
get_filename_component(_consumer_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_consumer_state "${_consumer_source_root}/resources/.state")
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _consumer_id)
set(_consumer_output "${_consumer_state}/consumer-${_consumer_id}")
if(EXISTS "${_consumer_output}" OR IS_SYMLINK "${_consumer_output}")
    message(FATAL_ERROR "The consumer test resource workspace already exists")
endif()
get_property(_tests DIRECTORY PROPERTY TESTS)
add_test(NAME iiMultimodalAgent.consumer_output_setup COMMAND "${CMAKE_COMMAND}"
    "-DRESOURCE_ROOT=${_consumer_state}" "-DOUTPUT_DIR=${_consumer_output}"
    -DACTION=setup -P "${_consumer_source_root}/tests/test_output_fixture.cmake")
add_test(NAME iiMultimodalAgent.consumer_output_cleanup COMMAND "${CMAKE_COMMAND}"
    "-DRESOURCE_ROOT=${_consumer_state}" "-DOUTPUT_DIR=${_consumer_output}"
    -DACTION=cleanup -P "${_consumer_source_root}/tests/test_output_fixture.cmake")
set_tests_properties(iiMultimodalAgent.consumer_output_setup PROPERTIES FIXTURES_SETUP AgentConsumerTestOutput)
set_tests_properties(iiMultimodalAgent.consumer_output_cleanup PROPERTIES FIXTURES_CLEANUP AgentConsumerTestOutput)
set_tests_properties(${_tests} PROPERTIES FIXTURES_REQUIRED AgentConsumerTestOutput)
set_tests_properties(${_tests} PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION
    "TMPDIR=set:${_consumer_output}/tmp;TMP=set:${_consumer_output}/tmp;TEMP=set:${_consumer_output}/tmp;IILOCALLLM_APP_ENDPOINTS=set:${_consumer_output}/tmp/app-endpoints")
set_tests_properties(iiMultimodalAgent.runtime_bindings PROPERTIES TIMEOUT 600)
if(APPLE)
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:" "DYLD_FRAMEWORK_PATH=unset:"
        "DYLD_FALLBACK_LIBRARY_PATH=unset:" "DYLD_VERSIONED_LIBRARY_PATH=unset:"
        "DYLD_VERSIONED_FRAMEWORK_PATH=unset:")
endif()

set_tests_properties(iiMultimodalAgent.installed_memory_dream PROPERTIES TIMEOUT 240)
