if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tests/question-c-consumer/main.c" PROPERTIES LANGUAGE CXX)
    add_executable(iiMultimodalAgentQuestionCAbiTests tests/question-c-consumer/main.c)
    target_link_libraries(iiMultimodalAgentQuestionCAbiTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.question_presentation_c COMMAND iiMultimodalAgentQuestionCAbiTests)
    set_tests_properties(iiMultimodalAgent.question_presentation_c PROPERTIES TIMEOUT 30)
    if(NOT CMAKE_CROSSCOMPILING AND NOT EMSCRIPTEN AND TARGET iiSDKCore::iiSDKCore)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME iiMultimodalAgent.question_presentation_python COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_question_presentation_python.py"
            "$<TARGET_FILE:iiMultimodalAgent>" "$<TARGET_FILE_DIR:iiSDKCore::iiSDKCore>")
        set_tests_properties(iiMultimodalAgent.question_presentation_python PROPERTIES TIMEOUT 30
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        add_test(NAME iiMultimodalAgent.question_presentation_installed COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/question-python-installed-consumer" "-DPYTHON=${Python3_EXECUTABLE}"
            "-DCORE_DIR=$<TARGET_FILE_DIR:iiSDKCore::iiSDKCore>" "-DCORE_PACKAGE_DIR=${iiSDKCore_DIR}"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DBUILD_CONFIG=$<CONFIG>" "-DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DINSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}" "-DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}"
            "-DQT_PACKAGE_DIR=${Qt6_DIR}" "-DLOCAL_LLM_DIR=${iiLocalLLM_DIR}" "-DDECISION_DIR=${iiDecision_DIR}"
            "-DLOCAL_DIFFUSION_DIR=${iiLocalDiffusion_DIR}" "-DAUDIO_DIR=${iiAudioGen_DIR}" "-DVOICE_DIR=${iiVoiceOver_DIR}"
            "-DFILE_PROVIDER_DIR=${iiFileProvider_DIR}" "-DFILE_PROVIDER_NATIVE_DIR=${iiFileProviderNative_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/question_presentation_install.cmake")
        set_tests_properties(iiMultimodalAgent.question_presentation_installed PROPERTIES TIMEOUT 90)
    endif()
    add_executable(iiMultimodalAgentNativeQuestionModelTests tests/native_question_view_model.cpp)
    target_link_libraries(iiMultimodalAgentNativeQuestionModelTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    if(APPLE)
        target_link_options(iiMultimodalAgentNativeQuestionModelTests BEFORE PRIVATE
            "LINKER:-rpath,$<TARGET_FILE_DIR:iiMultimodalAgent>")
    endif()
    add_test(NAME iiMultimodalAgent.native_question_model COMMAND iiMultimodalAgentNativeQuestionModelTests)
    if(APPLE)
        set_tests_properties(iiMultimodalAgent.native_question_model PROPERTIES ENVIRONMENT_MODIFICATION
            "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;DYLD_FALLBACK_FRAMEWORK_PATH=unset:")
    endif()
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _test_id)
    set(_test_state "${CMAKE_CURRENT_SOURCE_DIR}/resources/.state")
    set(_test_output "${_test_state}/tests-${_test_id}")
    include(tests/RuntimeBindings.cmake)
    add_executable(iiMultimodalAgentGraphQLTransportTests tests/graphql_transport_tests.cpp
        src/Agent/Integrations/GraphQLTransport.cpp)
    target_link_libraries(iiMultimodalAgentGraphQLTransportTests PRIVATE iiMultimodalAgent::iiMultimodalAgent iiNetwork::iiNetwork)
    add_test(NAME iiMultimodalAgent.graphql_transport COMMAND iiMultimodalAgentGraphQLTransportTests)
    set_tests_properties(iiMultimodalAgent.graphql_transport PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_executable(iiMultimodalAgentGraphQLTests tests/graphql_tests.cpp)
    target_compile_definitions(iiMultimodalAgentGraphQLTests PRIVATE
        IIMULTIMODALAGENT_TEST_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    target_link_libraries(iiMultimodalAgentGraphQLTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentGraphQLTests PROPERTIES AUTOMOC ON)
    if(UNIX)
        add_dependencies(iiMultimodalAgentGraphQLTests iiMultimodalAgentMcpServer iiMultimodalAgentD iimagent)
    endif()
    add_test(NAME iiMultimodalAgent.graphql COMMAND iiMultimodalAgentGraphQLTests)
    set_tests_properties(iiMultimodalAgent.graphql PROPERTIES TIMEOUT 240
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.graphql_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_graphql.py")
    add_executable(iiMultimodalAgentSemanticRagTests tests/semantic_rag_tests.cpp)
    target_link_libraries(iiMultimodalAgentSemanticRagTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.semantic_rag COMMAND iiMultimodalAgentSemanticRagTests)
    add_test(NAME iiMultimodalAgent.semantic_rag_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_semantic_rag.py")
    set_tests_properties(iiMultimodalAgent.semantic_rag PROPERTIES TIMEOUT 120)
    add_executable(iiMultimodalAgentRagTests tests/rag_tests.cpp)
    target_link_libraries(iiMultimodalAgentRagTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.rag COMMAND iiMultimodalAgentRagTests)
    add_test(NAME iiMultimodalAgent.foundation_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_foundation.py")
    set_tests_properties(iiMultimodalAgent.rag PROPERTIES TIMEOUT 120)
    option(IIMULTIMODALAGENT_TEST_RAG_INFERENCE "Enable explicitly requested RAG model execution" OFF)
    if(IIMULTIMODALAGENT_TEST_RAG_INFERENCE AND IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_CHAT_GGUF)
        add_executable(iiMultimodalAgentRagRuntimeSmoke tests/rag_runtime_smoke.cpp)
        target_link_libraries(iiMultimodalAgentRagRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
        add_test(NAME iiMultimodalAgent.rag_inference COMMAND iiMultimodalAgentRagRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
        set_tests_properties(iiMultimodalAgent.rag_inference PROPERTIES TIMEOUT 600 LABELS "inference")
    endif()
    add_executable(iiMultimodalAgentMultimodalConfigTests tests/multimodal_config_tests.cpp)
    target_link_libraries(iiMultimodalAgentMultimodalConfigTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.multimodal_config COMMAND iiMultimodalAgentMultimodalConfigTests)
    add_test(NAME iiMultimodalAgent.multimodal_config_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_multimodal_config.py")
    add_test(NAME iiMultimodalAgent.consumer_ctest_resources_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_consumer_ctest_resources.py")
    add_executable(iiMultimodalAgentMultimodalServicesTests tests/multimodal_services_tests.cpp)
    target_link_libraries(iiMultimodalAgentMultimodalServicesTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.multimodal_services COMMAND iiMultimodalAgentMultimodalServicesTests)
    add_test(NAME iiMultimodalAgent.multimodal_services_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_multimodal_services.py")
    add_test(NAME iiMultimodalAgent.voice_pcm_boundary COMMAND iiMultimodalAgentMultimodalServicesTests pcmUsesTheBoundAudioBackendAndRejectsInvalidOutput)
    set_tests_properties(iiMultimodalAgent.voice_pcm_boundary PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    add_executable(iiMultimodalAgentSpeechToolSmoke tests/speech_tool_smoke.cpp)
    target_link_libraries(iiMultimodalAgentSpeechToolSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.speech_backend_retired COMMAND iiMultimodalAgentSpeechToolSmoke)
    set_tests_properties(iiMultimodalAgent.speech_backend_retired PROPERTIES TIMEOUT 30)
    add_executable(iiMultimodalAgentServiceModelTests tests/service_model_tests.cpp)
    target_link_libraries(iiMultimodalAgentServiceModelTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentServiceModelTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.service_model COMMAND iiMultimodalAgentServiceModelTests)
    set_tests_properties(iiMultimodalAgent.service_model PROPERTIES WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.service_model_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_service_model.py")
    if(IIMULTIMODALAGENT_WITH_LLAMA)
    endif()
    add_executable(iiMultimodalAgentTeamTests tests/team_tests.cpp)
    target_link_libraries(iiMultimodalAgentTeamTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentTeamTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.teams COMMAND iiMultimodalAgentTeamTests)
    add_test(NAME iiMultimodalAgent.teams_offline COMMAND iiMultimodalAgentTeamTests messageSchemaPreservesConditionalSummary)
    set_tests_properties(iiMultimodalAgent.teams iiMultimodalAgent.teams_offline PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.teams_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_teams.py")
    add_executable(iiMultimodalAgentProcedureTests tests/procedure_tests.cpp)
    target_link_libraries(iiMultimodalAgentProcedureTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.procedures COMMAND iiMultimodalAgentProcedureTests)
    add_test(NAME iiMultimodalAgent.controls_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_controls.py")
    set_tests_properties(iiMultimodalAgent.procedures PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentDecisionGateTests tests/decision_gate_tests.cpp)
    target_link_libraries(iiMultimodalAgentDecisionGateTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.decision_gate COMMAND iiMultimodalAgentDecisionGateTests)
    add_executable(iiMultimodalAgentCTestSupportTests tests/ctest_support.cpp)
    target_compile_features(iiMultimodalAgentCTestSupportTests PRIVATE cxx_std_23)
    add_test(NAME iiMultimodalAgent.ctest_support COMMAND iiMultimodalAgentCTestSupportTests)
    add_test(NAME iiMultimodalAgent.decision_gate_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_decision_gate.py")
    add_executable(iiMultimodalAgentDecisionEngineTests tests/decision_engine_tests.cpp)
    target_link_libraries(iiMultimodalAgentDecisionEngineTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.decision_engine COMMAND iiMultimodalAgentDecisionEngineTests)
    add_executable(iiMultimodalAgentQtEventWaitTests tests/qt_event_wait_tests.cpp)
    target_compile_features(iiMultimodalAgentQtEventWaitTests PRIVATE cxx_std_23)
    target_link_libraries(iiMultimodalAgentQtEventWaitTests PRIVATE Qt6::Core)
    add_test(NAME iiMultimodalAgent.qt_event_wait COMMAND iiMultimodalAgentQtEventWaitTests)
    add_test(NAME iiMultimodalAgent.decision_engine_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_decision_engine.py")
    set_tests_properties(iiMultimodalAgent.decision_engine PROPERTIES TIMEOUT 90)
    set_tests_properties(iiMultimodalAgent.decision_gate PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentTeamNativeSmoke tests/team_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentTeamNativeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentTeamTaskNativeSmoke tests/team_task_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentTeamTaskNativeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentSessionForkTests tests/session_fork_tests.cpp)
    target_link_libraries(iiMultimodalAgentSessionForkTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.session_fork COMMAND iiMultimodalAgentSessionForkTests)
    add_test(NAME iiMultimodalAgent.session_fork_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_session_fork.py")
    set_tests_properties(iiMultimodalAgent.session_fork PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentFileCheckpointTests tests/file_checkpoint_tests.cpp)
    target_link_libraries(iiMultimodalAgentFileCheckpointTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.file_checkpoints COMMAND iiMultimodalAgentFileCheckpointTests)
    add_test(NAME iiMultimodalAgent.file_checkpoints_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_file_checkpoints.py")
    set_tests_properties(iiMultimodalAgent.file_checkpoints PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentNotebookRuntimeSmoke tests/notebook_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentNotebookRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentNotebookTests tests/notebook_tests.cpp)
    target_link_libraries(iiMultimodalAgentNotebookTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentNotebookTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.notebooks COMMAND iiMultimodalAgentNotebookTests)
    add_test(NAME iiMultimodalAgent.notebooks_offline COMMAND iiMultimodalAgentNotebookTests --offline)
    add_test(NAME iiMultimodalAgent.notebooks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_notebooks.py")
    set_tests_properties(iiMultimodalAgent.notebooks iiMultimodalAgent.notebooks_offline PROPERTIES
        TIMEOUT 90 WORKING_DIRECTORY "${_test_output}/tmp" SKIP_RETURN_CODE 77)
    add_executable(iiMultimodalAgentWorktreeRuntimeSmoke tests/worktree_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentWorktreeRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentWorktreeTests tests/worktree_tests.cpp)
    target_link_libraries(iiMultimodalAgentWorktreeTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentWorktreeTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.worktrees COMMAND iiMultimodalAgentWorktreeTests)
    set_tests_properties(iiMultimodalAgent.worktrees PROPERTIES TIMEOUT 180
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.worktrees_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_worktrees.py")
    add_executable(iiMultimodalAgentLspFixture tests/lsp_fixture.cpp)
    target_compile_features(iiMultimodalAgentLspFixture PRIVATE cxx_std_20)
    target_link_libraries(iiMultimodalAgentLspFixture PRIVATE Qt6::Core)
    add_executable(iiMultimodalAgentLspTests tests/lsp_tests.cpp)
    target_link_libraries(iiMultimodalAgentLspTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentLspTests PROPERTIES AUTOMOC ON)
    target_compile_definitions(iiMultimodalAgentLspTests PRIVATE LSP_FIXTURE="$<TARGET_FILE:iiMultimodalAgentLspFixture>")
    add_dependencies(iiMultimodalAgentLspTests iiMultimodalAgentLspFixture)
    add_test(NAME iiMultimodalAgent.lsp COMMAND iiMultimodalAgentLspTests)
    set_tests_properties(iiMultimodalAgent.lsp PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.lsp_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_lsp.py")
    add_executable(iiMultimodalAgentLspRuntimeSmoke tests/lsp_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentLspRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentWebFetchTests tests/web_fetch_tests.cpp)
    target_link_libraries(iiMultimodalAgentWebFetchTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentWebFetchTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.web_fetch COMMAND iiMultimodalAgentWebFetchTests)
    set_tests_properties(iiMultimodalAgent.web_fetch PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.web_fetch_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_web_fetch.py")
    if(IIMULTIMODALAGENT_WITH_LLAMA)
        add_executable(iiMultimodalAgentWebFetchRuntimeSmoke tests/web_fetch_runtime_smoke.cpp)
        target_link_libraries(iiMultimodalAgentWebFetchRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    endif()
    add_executable(iiMultimodalAgentWebFetchProbe tests/web_fetch_probe.cpp)
    target_link_libraries(iiMultimodalAgentWebFetchProbe PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentMultimodalTests tests/multimodal_tests.cpp)
    target_link_libraries(iiMultimodalAgentMultimodalTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.multimodal COMMAND iiMultimodalAgentMultimodalTests)
    add_test(NAME iiMultimodalAgent.multimodal_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_multimodal.py")
    set_tests_properties(iiMultimodalAgent.multimodal PROPERTIES TIMEOUT 60)
    if(IIMULTIMODALAGENT_WITH_LLAMA)
        add_executable(iiMultimodalAgentMultimodalRuntimeSmoke tests/multimodal_runtime_smoke.cpp)
        target_link_libraries(iiMultimodalAgentMultimodalRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
        foreach(_kind VISION AUDIO)
            if(_kind STREQUAL "VISION")
                set(_mode vision)
                set(_media "${IIMULTIMODALAGENT_TEST_IMAGE}")
            else()
                set(_mode audio)
                set(_media "${IIMULTIMODALAGENT_TEST_AUDIO}")
            endif()
            if(IIMULTIMODALAGENT_TEST_${_kind}_PACKAGE AND _media)
                add_test(NAME iiMultimodalAgent.multimodal_${_mode}_inference COMMAND iiMultimodalAgentMultimodalRuntimeSmoke
                    "${_mode}" "${IIMULTIMODALAGENT_TEST_${_kind}_PACKAGE}" "${_media}" "${_test_output}/multimodal-${_mode}.json")
                set_tests_properties(iiMultimodalAgent.multimodal_${_mode}_inference PROPERTIES TIMEOUT 240)
            endif()
        endforeach()
    endif()
    add_executable(iiMultimodalAgentMcpOutputHooksTests tests/mcp_output_hooks_tests.cpp)
    target_link_libraries(iiMultimodalAgentMcpOutputHooksTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentMcpOutputHooksTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.mcp_output_hooks COMMAND iiMultimodalAgentMcpOutputHooksTests)
    set_tests_properties(iiMultimodalAgent.mcp_output_hooks PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.mcp_output_hooks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_mcp_output_hooks.py")
    add_executable(iiMultimodalAgentAgentHooksTests tests/agent_hooks_tests.cpp)
    target_link_libraries(iiMultimodalAgentAgentHooksTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentAgentHooksTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.agent_hooks COMMAND iiMultimodalAgentAgentHooksTests)
    add_test(NAME iiMultimodalAgent.agent_hooks_offline COMMAND iiMultimodalAgentAgentHooksTests configurationDeduplicatesAgentAndPromptSeparately)
    set_tests_properties(iiMultimodalAgent.agent_hooks iiMultimodalAgent.agent_hooks_offline PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.agent_hooks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_hooks.py")
    add_executable(iiMultimodalAgentModelHooksTests tests/model_hooks_tests.cpp)
    target_link_libraries(iiMultimodalAgentModelHooksTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.model_hooks COMMAND iiMultimodalAgentModelHooksTests)
    set_tests_properties(iiMultimodalAgent.model_hooks PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentHttpHooksTests tests/http_hooks_tests.cpp)
    target_link_libraries(iiMultimodalAgentHttpHooksTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentHttpHooksTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.http_hooks COMMAND iiMultimodalAgentHttpHooksTests)
    set_tests_properties(iiMultimodalAgent.http_hooks PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.http_hooks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_http_hooks.py")
    add_executable(iiMultimodalAgentHttpHookProbe tests/http_hook_probe.cpp)
    target_link_libraries(iiMultimodalAgentHttpHookProbe PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_executable(iiMultimodalAgentInputQueueTests tests/input_queue_tests.cpp)
    target_link_libraries(iiMultimodalAgentInputQueueTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentInputQueueTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.input_queue COMMAND iiMultimodalAgentInputQueueTests)
    add_test(NAME iiMultimodalAgent.input_queue_offline COMMAND iiMultimodalAgentInputQueueTests orderedPersistentAndBounded)
    set_tests_properties(iiMultimodalAgent.input_queue iiMultimodalAgent.input_queue_offline PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.input_queue_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_input_queue.py")
    add_executable(iiMultimodalAgentPlanModeRuntimeSmoke tests/plan_mode_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentPlanModeRuntimeSmoke PRIVATE iiMultimodalAgent)
    if(IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_AGENT_CATALOG AND IIMULTIMODALAGENT_TEST_AGENT_MODEL)
        add_test(NAME iiMultimodalAgent.plan_mode_inference COMMAND iiMultimodalAgentPlanModeRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.plan_mode_inference PROPERTIES TIMEOUT 300 LABELS "inference")
    endif()
    add_executable(iiMultimodalAgentUserQuestionsRuntimeSmoke tests/user_questions_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentUserQuestionsRuntimeSmoke PRIVATE iiMultimodalAgent)
    add_executable(iiMultimodalAgentProjectMemoryRuntimeSmoke tests/project_memory_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentProjectMemoryRuntimeSmoke PRIVATE iiMultimodalAgent)
    add_executable(iiMultimodalAgentMemoryRecallRuntimeSmoke tests/memory_recall_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentMemoryRecallRuntimeSmoke PRIVATE iiMultimodalAgent)
    add_executable(iiMultimodalAgentMemoryExtractionRuntimeSmoke tests/memory_extraction_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentMemoryExtractionRuntimeSmoke PRIVATE iiMultimodalAgent)
    add_executable(iiMultimodalAgentSessionHistoryRuntimeSmoke tests/session_history_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentSessionHistoryRuntimeSmoke PRIVATE iiMultimodalAgent)
    add_executable(iiMultimodalAgentMemoryDreamRuntimeSmoke tests/memory_dream_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentMemoryDreamRuntimeSmoke PRIVATE iiMultimodalAgent)
    if(IIMULTIMODALAGENT_TEST_AGENT_CATALOG AND IIMULTIMODALAGENT_WITH_LLAMA)
        add_test(NAME iiMultimodalAgent.user_questions_inference COMMAND iiMultimodalAgentUserQuestionsRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.user_questions_inference PROPERTIES TIMEOUT 300 LABELS "inference")
        add_test(NAME iiMultimodalAgent.project_memory_inference COMMAND iiMultimodalAgentProjectMemoryRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.project_memory_inference PROPERTIES TIMEOUT 600 LABELS "inference")
        add_test(NAME iiMultimodalAgent.memory_recall_inference COMMAND iiMultimodalAgentMemoryRecallRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.memory_recall_inference PROPERTIES TIMEOUT 600 LABELS "inference")
        add_test(NAME iiMultimodalAgent.memory_extraction_inference COMMAND iiMultimodalAgentMemoryExtractionRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.memory_extraction_inference PROPERTIES TIMEOUT 600 LABELS "inference")
        add_test(NAME iiMultimodalAgent.session_history_inference COMMAND iiMultimodalAgentSessionHistoryRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.session_history_inference PROPERTIES TIMEOUT 600 LABELS "inference")
        add_test(NAME iiMultimodalAgent.memory_dream_inference COMMAND iiMultimodalAgentMemoryDreamRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
        set_tests_properties(iiMultimodalAgent.memory_dream_inference PROPERTIES TIMEOUT 600 LABELS "inference")
    endif()
    add_test(NAME iiMultimodalAgent.question_sheet_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_question_sheet.py")
    if(IIMULTIMODALAGENT_TEST_LVRS_DIR)
        find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Quick QuickControls2)
        find_package(LVRS CONFIG REQUIRED PATHS "${IIMULTIMODALAGENT_TEST_LVRS_DIR}/lib/cmake/LVRS" NO_DEFAULT_PATH)
        add_executable(iiMultimodalAgentQuestionSheetTests tests/Legacy/question_sheet_tests.cpp)
        target_link_libraries(iiMultimodalAgentQuestionSheetTests PRIVATE iiMultimodalAgent Qt6::Quick LVRS::LVRS)
        target_compile_definitions(iiMultimodalAgentQuestionSheetTests PRIVATE
            QUESTION_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/Legacy/qml"
            QUESTION_LVRS_IMPORT_PATH="${IIMULTIMODALAGENT_TEST_LVRS_DIR}/lib/qt6/qml")
        add_test(NAME iiMultimodalAgent.question_sheet COMMAND iiMultimodalAgentQuestionSheetTests)
        set_tests_properties(iiMultimodalAgent.question_sheet PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY "${_test_output}/tmp"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_DISABLE_DISK_CACHE=1;IIMULTIMODALAGENT_DISABLE_APP_MCP=1")
    endif()
    add_executable(iiMultimodalAgentQuestionInboxTests tests/question_inbox_tests.cpp)
    target_link_libraries(iiMultimodalAgentQuestionInboxTests PRIVATE iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentQuestionInboxTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.question_inbox COMMAND iiMultimodalAgentQuestionInboxTests)
    set_tests_properties(iiMultimodalAgent.question_inbox PROPERTIES WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.question_inbox_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_question_inbox.py")
    add_executable(iiMultimodalAgentSignalRecorderTests tests/signal_recorder_tests.cpp)
    target_link_libraries(iiMultimodalAgentSignalRecorderTests PRIVATE Qt6::Core)
    set_target_properties(iiMultimodalAgentSignalRecorderTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.signal_recorder COMMAND iiMultimodalAgentSignalRecorderTests)
    add_executable(iiMultimodalAgentProjectMemoryTests tests/project_memory_tests.cpp)
    target_link_libraries(iiMultimodalAgentProjectMemoryTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.project_memory COMMAND iiMultimodalAgentProjectMemoryTests)
    add_test(NAME iiMultimodalAgent.project_memory_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_project_memory.py")
    add_executable(iiMultimodalAgentProjectMemoryEngineTests tests/project_memory_engine_tests.cpp)
    target_link_libraries(iiMultimodalAgentProjectMemoryEngineTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.project_memory_engine COMMAND iiMultimodalAgentProjectMemoryEngineTests)
    add_test(NAME iiMultimodalAgent.project_memory_engine_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_project_memory_engine.py")
    add_executable(iiMultimodalAgentMemoryRecallTests tests/memory_recall_tests.cpp)
    target_link_libraries(iiMultimodalAgentMemoryRecallTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.memory_recall COMMAND iiMultimodalAgentMemoryRecallTests)
    add_test(NAME iiMultimodalAgent.memory_recall_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_memory_recall.py")
    set_tests_properties(iiMultimodalAgent.memory_recall PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentMemoryRecallEngineTests tests/memory_recall_engine_tests.cpp)
    target_link_libraries(iiMultimodalAgentMemoryRecallEngineTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.memory_recall_engine COMMAND iiMultimodalAgentMemoryRecallEngineTests)
    add_test(NAME iiMultimodalAgent.memory_recall_engine_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_memory_recall_engine.py")
    set_tests_properties(iiMultimodalAgent.memory_recall_engine PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentSessionHistoryTests tests/session_history_tests.cpp)
    add_executable(iiMultimodalAgentMemoryDreamTests tests/memory_dream_tests.cpp)
    target_link_libraries(iiMultimodalAgentMemoryDreamTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.memory_dream COMMAND iiMultimodalAgentMemoryDreamTests)
    add_test(NAME iiMultimodalAgent.memory_dream_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_memory_dream.py")
    set_tests_properties(iiMultimodalAgent.memory_dream PROPERTIES TIMEOUT 240)
    add_executable(iiMultimodalAgentMemoryDreamEngineTests tests/memory_dream_engine_tests.cpp)
    target_link_libraries(iiMultimodalAgentMemoryDreamEngineTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.memory_dream_engine COMMAND iiMultimodalAgentMemoryDreamEngineTests)
    add_test(NAME iiMultimodalAgent.memory_dream_engine_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_memory_dream_engine.py")
    set_tests_properties(iiMultimodalAgent.memory_dream_engine PROPERTIES TIMEOUT 90)
    target_link_libraries(iiMultimodalAgentSessionHistoryTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.session_history COMMAND iiMultimodalAgentSessionHistoryTests)
    add_test(NAME iiMultimodalAgent.session_history_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_session_history.py")
    set_tests_properties(iiMultimodalAgent.session_history PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentSessionHistoryEngineTests tests/session_history_engine_tests.cpp)
    target_link_libraries(iiMultimodalAgentSessionHistoryEngineTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.session_history_engine COMMAND iiMultimodalAgentSessionHistoryEngineTests)
    add_test(NAME iiMultimodalAgent.session_history_engine_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_session_history_engine.py")
    set_tests_properties(iiMultimodalAgent.session_history_engine PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentMemoryExtractionTests tests/memory_extraction_tests.cpp)
    target_link_libraries(iiMultimodalAgentMemoryExtractionTests PRIVATE iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentMemoryExtractionTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.memory_extraction COMMAND iiMultimodalAgentMemoryExtractionTests)
    add_test(NAME iiMultimodalAgent.memory_extraction_offline COMMAND iiMultimodalAgentMemoryExtractionTests --offline)
    add_test(NAME iiMultimodalAgent.memory_extraction_fake_model COMMAND iiMultimodalAgentMemoryExtractionTests identicalParentPrefixNativeWriteAndNoForkTranscript)
    set_tests_properties(iiMultimodalAgent.memory_extraction iiMultimodalAgent.memory_extraction_offline iiMultimodalAgent.memory_extraction_fake_model PROPERTIES
        TIMEOUT 90 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.memory_extraction_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_memory_extraction.py")
    add_executable(iiMultimodalAgentMemoryExtractionEngineTests tests/memory_extraction_engine_tests.cpp)
    target_link_libraries(iiMultimodalAgentMemoryExtractionEngineTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.memory_extraction_engine COMMAND iiMultimodalAgentMemoryExtractionEngineTests)
    add_test(NAME iiMultimodalAgent.memory_extraction_engine_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_memory_extraction_engine.py")
    set_tests_properties(iiMultimodalAgent.memory_extraction_engine PROPERTIES TIMEOUT 90)
    set_tests_properties(iiMultimodalAgent.project_memory iiMultimodalAgent.project_memory_engine PROPERTIES TIMEOUT 90)
    set_tests_properties(iiMultimodalAgent.question_inbox PROPERTIES TIMEOUT 60)
    add_executable(iiMultimodalAgentUserQuestionsTests tests/user_questions_tests.cpp)
    target_link_libraries(iiMultimodalAgentUserQuestionsTests PRIVATE iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentUserQuestionsTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.user_questions COMMAND iiMultimodalAgentUserQuestionsTests)
    add_test(NAME iiMultimodalAgent.user_questions_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_user_questions.py")
    set_tests_properties(iiMultimodalAgent.user_questions PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentPlanModeTests tests/plan_mode_tests.cpp)
    target_link_libraries(iiMultimodalAgentPlanModeTests PRIVATE iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentPlanModeTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.plan_mode COMMAND iiMultimodalAgentPlanModeTests)
    add_test(NAME iiMultimodalAgent.plan_mode_offline COMMAND iiMultimodalAgentPlanModeTests explicitDenyAndAskStillApplyToPlanWrites)
    set_tests_properties(iiMultimodalAgent.plan_mode iiMultimodalAgent.plan_mode_offline PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.plan_mode_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_plan_mode.py")
    add_executable(iiMultimodalAgentInputLifecycleTests tests/input_lifecycle_tests.cpp)
    target_link_libraries(iiMultimodalAgentInputLifecycleTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.input_lifecycle COMMAND iiMultimodalAgentInputLifecycleTests)
    add_test(NAME iiMultimodalAgent.input_lifecycle_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_input_lifecycle.py")
    set_tests_properties(iiMultimodalAgent.input_lifecycle PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentSessionEndTests tests/session_end_tests.cpp)
    target_link_libraries(iiMultimodalAgentSessionEndTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentSessionEndTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.session_end COMMAND iiMultimodalAgentSessionEndTests)
    add_test(NAME iiMultimodalAgent.session_end_offline COMMAND iiMultimodalAgentSessionEndTests --offline)
    set_tests_properties(iiMultimodalAgent.session_end iiMultimodalAgent.session_end_offline PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_test(NAME iiMultimodalAgent.session_end_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_session_end.py")
    add_executable(iiMultimodalAgentSessionClearTests tests/session_clear_tests.cpp)
    target_link_libraries(iiMultimodalAgentSessionClearTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.session_clear COMMAND iiMultimodalAgentSessionClearTests)
    add_test(NAME iiMultimodalAgent.session_clear_offline COMMAND iiMultimodalAgentSessionClearTests --offline)
    add_test(NAME iiMultimodalAgent.session_clear_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_session_clear.py")
    set_tests_properties(iiMultimodalAgent.session_clear iiMultimodalAgent.session_clear_offline PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(iiMultimodalAgent.session_clear PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentPermissionRequestTests tests/permission_request_tests.cpp)
    target_link_libraries(iiMultimodalAgentPermissionRequestTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.permission_request COMMAND iiMultimodalAgentPermissionRequestTests)
    add_test(NAME iiMultimodalAgent.permission_request_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_permission_request.py")
    set_tests_properties(iiMultimodalAgent.permission_request PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentPermissionUpdatesTests tests/permission_updates_tests.cpp)
    target_link_libraries(iiMultimodalAgentPermissionUpdatesTests PRIVATE iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentPermissionUpdatesTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.permission_updates COMMAND iiMultimodalAgentPermissionUpdatesTests)
    set_tests_properties(iiMultimodalAgent.permission_updates PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_test(NAME iiMultimodalAgent.permission_updates_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_permission_updates.py")
    add_executable(iiMultimodalAgentPermissionRequestsTests tests/permission_requests_tests.cpp)
    target_link_libraries(iiMultimodalAgentPermissionRequestsTests PRIVATE iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.permission_requests COMMAND iiMultimodalAgentPermissionRequestsTests)
    set_tests_properties(iiMultimodalAgent.permission_requests PROPERTIES TIMEOUT 90)
    add_executable(iiMultimodalAgentShellTaskTests tests/shell_tasks_tests.cpp)
    target_link_libraries(iiMultimodalAgentShellTaskTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentShellTaskTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.shell_tasks COMMAND iiMultimodalAgentShellTaskTests)
    set_tests_properties(iiMultimodalAgent.shell_tasks PROPERTIES TIMEOUT 90
        SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.shell_tasks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_shell_tasks.py")
    add_executable(iiMultimodalAgentTaskTests tests/task_tests.cpp)
    target_link_libraries(iiMultimodalAgentTaskTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentTaskTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.tasks COMMAND iiMultimodalAgentTaskTests)
    add_test(NAME iiMultimodalAgent.tasks_offline COMMAND iiMultimodalAgentTaskTests unicodeOwnerSurvivesPersistentReload)
    set_tests_properties(iiMultimodalAgent.tasks iiMultimodalAgent.tasks_offline PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.tasks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_tasks.py")
    add_executable(iiMultimodalAgentLocalApplicationTests tests/local_applications_tests.cpp)
    target_link_libraries(iiMultimodalAgentLocalApplicationTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentLocalApplicationTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.local_applications COMMAND iiMultimodalAgentLocalApplicationTests)
    add_test(NAME iiMultimodalAgent.local_applications_offline COMMAND iiMultimodalAgentLocalApplicationTests
        objectThreadAndOwnerLifetime)
    set_tests_properties(iiMultimodalAgent.local_applications iiMultimodalAgent.local_applications_offline
        PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.local_applications_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_local_applications.py")
    add_executable(iiMultimodalAgentDiscoveryTests tests/discovery_tests.cpp)
    target_link_libraries(iiMultimodalAgentDiscoveryTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentDiscoveryTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.discovery COMMAND iiMultimodalAgentDiscoveryTests)
    add_test(NAME iiMultimodalAgent.discovery_offline COMMAND iiMultimodalAgentDiscoveryTests keywordAndDirectSearch)
    set_tests_properties(iiMultimodalAgent.discovery iiMultimodalAgent.discovery_offline PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.discovery_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_discovery.py")
    add_executable(iiMultimodalAgentCompactionTests tests/compaction_tests.cpp)
    target_link_libraries(iiMultimodalAgentCompactionTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentCompactionTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.compaction COMMAND iiMultimodalAgentCompactionTests)
    add_test(NAME iiMultimodalAgent.compaction_offline COMMAND iiMultimodalAgentCompactionTests
        versionOneMigrationAndTornCheckpoint)
    set_tests_properties(iiMultimodalAgent.compaction iiMultimodalAgent.compaction_offline PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.compaction_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_compaction.py")
    add_executable(iiMultimodalAgentContextTests tests/context_tests.cpp)
    target_link_libraries(iiMultimodalAgentContextTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.context COMMAND iiMultimodalAgentContextTests)
    set_tests_properties(iiMultimodalAgent.context PROPERTIES TIMEOUT 60)
    add_executable(iiMultimodalAgentAgentTests tests/agent_tests.cpp tests/agent_tests.h)
    target_link_libraries(iiMultimodalAgentAgentTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentAgentTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.agent COMMAND iiMultimodalAgentAgentTests)
    add_test(NAME iiMultimodalAgent.agent_offline COMMAND iiMultimodalAgentAgentTests schemaAndRegistry)
    set_tests_properties(iiMultimodalAgent.agent iiMultimodalAgent.agent_offline PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.agent_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_engine.py")
    add_executable(iiMultimodalAgentAgentApiTests tests/agent_api_tests.cpp)
    target_link_libraries(iiMultimodalAgentAgentApiTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentAgentApiTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.agent_api COMMAND iiMultimodalAgentAgentApiTests)
    add_test(NAME iiMultimodalAgent.agent_api_offline COMMAND iiMultimodalAgentAgentApiTests
        permissionsInspectionIsAuthenticatedAndCannotChangePolicy)
    set_tests_properties(iiMultimodalAgent.agent_api iiMultimodalAgent.agent_api_offline PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.agent_api_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_api.py")
    add_executable(iiMultimodalAgentSkillTests tests/skill_tests.cpp)
    target_link_libraries(iiMultimodalAgentSkillTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.skills COMMAND iiMultimodalAgentSkillTests)
    add_executable(iiMultimodalAgentPluginPeer tests/plugin_peer.cpp)
    target_link_libraries(iiMultimodalAgentPluginPeer PRIVATE Qt6::Core)
    add_executable(iiMultimodalAgentPluginTests tests/plugin_tests.cpp)
    target_link_libraries(iiMultimodalAgentPluginTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentPluginTests PROPERTIES AUTOMOC ON)
    target_compile_definitions(iiMultimodalAgentPluginTests PRIVATE PLUGIN_PEER="$<TARGET_FILE:iiMultimodalAgentPluginPeer>" LSP_FIXTURE="$<TARGET_FILE:iiMultimodalAgentLspFixture>" PLUGIN_CLI="$<TARGET_FILE:iiMultimodalAgentPlugins>")
    add_dependencies(iiMultimodalAgentPluginTests iiMultimodalAgentPluginPeer iiMultimodalAgentLspFixture iiMultimodalAgentPlugins)
    add_test(NAME iiMultimodalAgent.plugins COMMAND iiMultimodalAgentPluginTests)
    add_test(NAME iiMultimodalAgent.plugins_offline COMMAND iiMultimodalAgentPluginTests --offline)
    set_tests_properties(iiMultimodalAgent.plugins iiMultimodalAgent.plugins_offline PROPERTIES
        TIMEOUT 90 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.plugins_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_plugins.py")
    add_executable(iiMultimodalAgentPluginRuntimeSmoke tests/plugin_runtime_smoke.cpp)
    target_link_libraries(iiMultimodalAgentPluginRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.plugin_runtime COMMAND iiMultimodalAgentPluginRuntimeSmoke "$<TARGET_FILE:iiMultimodalAgentPluginPeer>")
    set_tests_properties(iiMultimodalAgent.plugin_runtime PROPERTIES TIMEOUT 60)
    add_executable(iiMultimodalAgentSkillForkTests tests/skill_fork_tests.cpp)
    target_link_libraries(iiMultimodalAgentSkillForkTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.skill_fork COMMAND iiMultimodalAgentSkillForkTests)
    add_test(NAME iiMultimodalAgent.skill_fork_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_skill_fork.py")
    add_executable(iiMultimodalAgentPermissionRuleTests tests/permission_rules_tests.cpp)
    target_link_libraries(iiMultimodalAgentPermissionRuleTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentPermissionRuleTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.permission_rules COMMAND iiMultimodalAgentPermissionRuleTests)
    set_tests_properties(iiMultimodalAgent.permission_rules PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME iiMultimodalAgent.permission_rules_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_permission_rules.py")
    add_executable(iiMultimodalAgentPermissionSettingsTests tests/permission_settings_tests.cpp)
    target_link_libraries(iiMultimodalAgentPermissionSettingsTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    target_compile_definitions(iiMultimodalAgentPermissionSettingsTests PRIVATE IIMULTIMODAL_PERMISSION_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME iiMultimodalAgent.permission_settings COMMAND iiMultimodalAgentPermissionSettingsTests)
    add_test(NAME iiMultimodalAgent.permission_settings_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_permission_settings.py")
    add_executable(iiMultimodalAgentWorkingDirectoriesTests tests/working_directories_tests.cpp)
    target_link_libraries(iiMultimodalAgentWorkingDirectoriesTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentWorkingDirectoriesTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.working_directories COMMAND iiMultimodalAgentWorkingDirectoriesTests)
    add_test(NAME iiMultimodalAgent.working_directories_offline COMMAND iiMultimodalAgentWorkingDirectoriesTests --offline)
    set_tests_properties(iiMultimodalAgent.working_directories iiMultimodalAgent.working_directories_offline PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME iiMultimodalAgent.working_directories_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_working_directories.py")
    if(UNIX AND NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
        add_executable(iiMultimodalAgentAsyncHooksTests tests/async_hooks_tests.cpp)
        target_link_libraries(iiMultimodalAgentAsyncHooksTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
        set_target_properties(iiMultimodalAgentAsyncHooksTests PROPERTIES AUTOMOC ON)
        add_test(NAME iiMultimodalAgent.async_hooks COMMAND iiMultimodalAgentAsyncHooksTests)
        set_tests_properties(iiMultimodalAgent.async_hooks PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY "${_test_output}/tmp")
        add_test(NAME iiMultimodalAgent.async_hooks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_async_hooks.py")
        add_executable(iiMultimodalAgentAsyncHookRuntimeSmoke tests/async_hook_runtime_smoke.cpp)
        target_link_libraries(iiMultimodalAgentAsyncHookRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
        if(IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_CHAT_GGUF)
            add_test(NAME iiMultimodalAgent.async_hooks_inference COMMAND iiMultimodalAgentAsyncHookRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
            set_tests_properties(iiMultimodalAgent.async_hooks_inference PROPERTIES TIMEOUT 300 LABELS "inference")
        endif()
        add_executable(iiMultimodalAgentCommandHooksTests tests/command_hooks_tests.cpp)
        target_link_libraries(iiMultimodalAgentCommandHooksTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
        set_target_properties(iiMultimodalAgentCommandHooksTests PROPERTIES AUTOMOC ON)
        add_test(NAME iiMultimodalAgent.command_hooks COMMAND iiMultimodalAgentCommandHooksTests)
        set_tests_properties(iiMultimodalAgent.command_hooks PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY "${_test_output}/tmp")
        add_test(NAME iiMultimodalAgent.command_hooks_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_command_hooks.py")
    endif()
    add_executable(iiMultimodalAgentSkillPermissionTests tests/skill_permission_tests.cpp)
    target_link_libraries(iiMultimodalAgentSkillPermissionTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.skill_permissions COMMAND iiMultimodalAgentSkillPermissionTests)
    add_test(NAME iiMultimodalAgent.skill_permissions_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_skill_permission.py")
    set_tests_properties(iiMultimodalAgent.skills PROPERTIES TIMEOUT 60)
    add_executable(iiMultimodalAgentAgentProfileTests tests/agent_profiles_tests.cpp)
    target_link_libraries(iiMultimodalAgentAgentProfileTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    add_test(NAME iiMultimodalAgent.agent_profiles COMMAND iiMultimodalAgentAgentProfileTests)
    add_test(NAME iiMultimodalAgent.agent_profiles_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_profiles.py")
    set_tests_properties(iiMultimodalAgent.agent_profiles PROPERTIES TIMEOUT 60)
    add_executable(iiMultimodalAgentSubagentTests tests/subagent_tests.cpp)
    target_link_libraries(iiMultimodalAgentSubagentTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentSubagentTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.subagents COMMAND iiMultimodalAgentSubagentTests)
    add_test(NAME iiMultimodalAgent.subagents_offline COMMAND iiMultimodalAgentSubagentTests
        expandedForkRequestIsValidatedBeforeSavingChild)
    set_tests_properties(iiMultimodalAgent.subagents iiMultimodalAgent.subagents_offline PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.subagent_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_subagent.py")
    add_executable(iiMultimodalAgentAgentTransportTests tests/agent_transport_tests.cpp)
    target_compile_definitions(iiMultimodalAgentAgentTransportTests PRIVATE
        IIMULTIMODALAGENT_TEST_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    target_link_libraries(iiMultimodalAgentAgentTransportTests PRIVATE iiMultimodalAgent::iiMultimodalAgent Qt6::Network)
    set_target_properties(iiMultimodalAgentAgentTransportTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.agent_transport COMMAND iiMultimodalAgentAgentTransportTests)
    set_tests_properties(iiMultimodalAgent.agent_transport PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY "${_test_output}/tmp")
    foreach(case IN ITEMS
        hostQuantitiesCrossHttpAndDecisionResultCrossesIpc
        procedureReturnsCrossIpcAndHttpWithoutBlockingControls
        authSessionsForkAndEventOrder
        crossTransportCancellationAndDisconnect
        timeoutAndShutdown
        streamOverflowHasIndependentDeadline)
        add_test(NAME "iiMultimodalAgent.agent_transport.${case}"
            COMMAND iiMultimodalAgentAgentTransportTests "${case}")
        set_tests_properties("iiMultimodalAgent.agent_transport.${case}" PROPERTIES
            TIMEOUT 60 WORKING_DIRECTORY "${_test_output}/tmp")
    endforeach()
    add_test(NAME iiMultimodalAgent.agent_transport_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_agent_transport.py")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" Python3_EXECUTABLE)
    add_test(NAME iiMultimodalAgent.rag_cli COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/rag_cli.py $<TARGET_FILE:iimagent-rag>)
    set_tests_properties(iiMultimodalAgent.rag_cli PROPERTIES TIMEOUT 120)
    add_test(NAME iiMultimodalAgent.web_fetch_tls COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/web_fetch_tls.py $<TARGET_FILE:iiMultimodalAgentWebFetchProbe>)
    set_tests_properties(iiMultimodalAgent.web_fetch_tls PROPERTIES TIMEOUT 90)
    set(_versioned_executables)
    foreach(_entrypoint IN LISTS _versioned_entrypoints)
        list(APPEND _versioned_executables $<TARGET_FILE:${_entrypoint}>)
    endforeach()
    add_test(NAME iiMultimodalAgent.executable_versions COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/executable_versions.py ${PROJECT_VERSION} ${_versioned_executables})
    add_test(NAME iiMultimodalAgent.http_hooks_tls COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_hooks_tls.py $<TARGET_FILE:iiMultimodalAgentHttpHookProbe>)
    set_tests_properties(iiMultimodalAgent.http_hooks_tls PROPERTIES TIMEOUT 90)
    target_compile_definitions(iiMultimodalAgentDiscoveryTests PRIVATE MCP_TEST_PYTHON="${Python3_EXECUTABLE}" MCP_TEST_PEER="${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_peer.py")
    add_executable(iiMultimodalAgentMcpHttpTests tests/mcp_http_tests.cpp)
    target_link_libraries(iiMultimodalAgentMcpHttpTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    target_compile_definitions(iiMultimodalAgentMcpHttpTests PRIVATE MCP_TEST_PYTHON="${Python3_EXECUTABLE}" MCP_HTTP_PEER="${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_http_peer.py")
    set_target_properties(iiMultimodalAgentMcpHttpTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.mcp_http_client COMMAND iiMultimodalAgentMcpHttpTests)
    set_tests_properties(iiMultimodalAgent.mcp_http_client PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME iiMultimodalAgent.mcp_http_client_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_mcp_http_client.py")
    add_executable(iiMultimodalAgentMcpHttpServerTests tests/mcp_http_server_tests.cpp)
    target_link_libraries(iiMultimodalAgentMcpHttpServerTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentMcpHttpServerTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.mcp_http_server COMMAND iiMultimodalAgentMcpHttpServerTests)
    set_tests_properties(iiMultimodalAgent.mcp_http_server PROPERTIES TIMEOUT 60)
    add_test(NAME iiMultimodalAgent.mcp_http_server_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_mcp_http_server.py")
    add_executable(iiMultimodalAgentMcpHttpServerFixture tests/mcp_http_server_fixture.cpp)
    target_link_libraries(iiMultimodalAgentMcpHttpServerFixture PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    if(Python3_EXECUTABLE)
        add_test(NAME iiMultimodalAgent.mcp_http_server_wire COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_http_server_wire.py" $<TARGET_FILE:iiMultimodalAgentMcpHttpServerFixture>)
        set_tests_properties(iiMultimodalAgent.mcp_http_server_wire PROPERTIES TIMEOUT 90)
    endif()
    add_executable(iiMultimodalAgentMcpTests tests/mcp_tests.cpp)
    target_link_libraries(iiMultimodalAgentMcpTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    target_compile_definitions(iiMultimodalAgentMcpTests PRIVATE MCP_TEST_PYTHON="${Python3_EXECUTABLE}" MCP_TEST_PEER="${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_peer.py")
    set_target_properties(iiMultimodalAgentMcpTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.mcp_stdio COMMAND iiMultimodalAgentMcpTests)
    add_test(NAME iiMultimodalAgent.mcp_stdio_offline COMMAND iiMultimodalAgentMcpTests nativeAdapterDoesNotDropMedia)
    set_tests_properties(iiMultimodalAgent.mcp_stdio iiMultimodalAgent.mcp_stdio_offline PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.mcp_stdio_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_mcp_stdio.py")
    add_executable(iiMultimodalAgentMcpServerTests tests/mcp_server_tests.cpp)
    target_link_libraries(iiMultimodalAgentMcpServerTests PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    set_target_properties(iiMultimodalAgentMcpServerTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiMultimodalAgent.mcp_server COMMAND iiMultimodalAgentMcpServerTests)
    set_tests_properties(iiMultimodalAgent.mcp_server PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY "${_test_output}/tmp")
    add_test(NAME iiMultimodalAgent.mcp_session_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_mcp_session.py")
    if(UNIX)
        add_test(NAME iiMultimodalAgent.mcp_http_server_cli COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_http_server_cli.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>)
        set_tests_properties(iiMultimodalAgent.mcp_http_server_cli PROPERTIES TIMEOUT 60)
        add_test(NAME iiMultimodalAgent.mcp_server_transport COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_transport.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>)
        set_tests_properties(iiMultimodalAgent.mcp_server_transport PROPERTIES TIMEOUT 60)
    endif()
    if(IIMULTIMODALAGENT_TEST_MCP_PYTHON)
        add_executable(iiMultimodalAgentMcpOfficialSmoke tests/mcp_official_smoke.cpp)
        target_link_libraries(iiMultimodalAgentMcpOfficialSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
        add_test(NAME iiMultimodalAgent.mcp_official COMMAND iiMultimodalAgentMcpOfficialSmoke
            "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py")
        set_tests_properties(iiMultimodalAgent.mcp_official PROPERTIES TIMEOUT 60)
        add_test(NAME iiMultimodalAgent.mcp_http_official COMMAND iiMultimodalAgentMcpOfficialSmoke
            "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py" streamable-http)
        set_tests_properties(iiMultimodalAgent.mcp_http_official PROPERTIES TIMEOUT 60)
        if(UNIX)
            add_test(NAME iiMultimodalAgent.mcp_server_official COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>
                --report "${_test_output}/mcp-server-official-result.json")
            set_tests_properties(iiMultimodalAgent.mcp_server_official PROPERTIES TIMEOUT 60)
            add_test(NAME iiMultimodalAgent.mcp_http_server_official COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>
                --http --report "${_test_output}/mcp-http-server-official-result.json")
            set_tests_properties(iiMultimodalAgent.mcp_http_server_official PROPERTIES TIMEOUT 60)
        endif()
    endif()
    if(IIMULTIMODALAGENT_TEST_MCP_PYTHON AND TARGET iiMultimodalAgentMcpServer)
        add_test(NAME iiMultimodalAgent.inputs_mcp_stdio COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer> --inputs)
        add_test(NAME iiMultimodalAgent.inputs_mcp_http COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer> --inputs --http)
        set_tests_properties(iiMultimodalAgent.inputs_mcp_stdio iiMultimodalAgent.inputs_mcp_http PROPERTIES TIMEOUT 90)
        add_test(NAME iiMultimodalAgent.discovery_mcp_stdio COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer> --managed)
        add_test(NAME iiMultimodalAgent.discovery_mcp_http COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer> --managed --http)
        set_tests_properties(iiMultimodalAgent.discovery_mcp_stdio iiMultimodalAgent.discovery_mcp_http PROPERTIES TIMEOUT 90)
    endif()
    if(UNIX)
        set(_cli_fixture)
        if(IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_GGUF)
            set(_cli_fixture "${IIMULTIMODALAGENT_TEST_GGUF}")
        endif()
        add_test(NAME iiMultimodalAgent.agent_api_daemon COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_api_smoke.py
            $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> --report "${_test_output}/agent-api-daemon-result.json")
        set_tests_properties(iiMultimodalAgent.agent_api_daemon PROPERTIES TIMEOUT 180)
        if(TARGET iiMultimodalAgentMcpServer)
            if(NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
                add_test(NAME iiMultimodalAgent.command_hooks_wire COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/command_hooks_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --report ${_test_output}/command-hooks-wire.json)
                set_tests_properties(iiMultimodalAgent.command_hooks_wire PROPERTIES TIMEOUT 180)
                add_test(NAME iiMultimodalAgent.plan_mode_wire COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/plan_mode_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --report ${_test_output}/plan-mode-wire.json)
                set_tests_properties(iiMultimodalAgent.plan_mode_wire PROPERTIES TIMEOUT 90)
                add_test(NAME iiMultimodalAgent.user_questions_wire COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/user_questions_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --report ${_test_output}/user-questions-wire.json)
                set_tests_properties(iiMultimodalAgent.user_questions_wire PROPERTIES TIMEOUT 120)
                add_test(NAME iiMultimodalAgent.project_memory_wire COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_memory_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --report ${_test_output}/project-memory-wire.json)
                set_tests_properties(iiMultimodalAgent.project_memory_wire PROPERTIES TIMEOUT 120)
                add_test(NAME iiMultimodalAgent.async_hooks_wire COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/async_hooks_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --report ${_test_output}/async-hooks-wire.json)
                set_tests_properties(iiMultimodalAgent.async_hooks_wire PROPERTIES TIMEOUT 90)
                add_test(NAME iiMultimodalAgent.http_hooks_wire COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/command_hooks_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --http-hooks --report ${_test_output}/http-hooks-wire.json)
                set_tests_properties(iiMultimodalAgent.http_hooks_wire PROPERTIES TIMEOUT 180)
                add_test(NAME iiMultimodalAgent.model_hooks_cli COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_hooks_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --report ${_test_output}/model-hooks-cli.json)
                set_tests_properties(iiMultimodalAgent.model_hooks_cli PROPERTIES TIMEOUT 90)
                add_test(NAME iiMultimodalAgent.agent_hooks_cli COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_hooks_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --agent-hooks --report ${_test_output}/agent-hooks-cli.json)
                set_tests_properties(iiMultimodalAgent.agent_hooks_cli PROPERTIES TIMEOUT 90)
            endif()
            add_test(NAME iiMultimodalAgent.permission_settings_wire COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/permission_settings_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                --report ${_test_output}/permission-settings-wire.json)
            set_tests_properties(iiMultimodalAgent.permission_settings_wire PROPERTIES TIMEOUT 180)
            add_test(NAME iiMultimodalAgent.permission_requests_wire COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/permission_requests_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                --report ${_test_output}/permission-requests-wire.json)
            set_tests_properties(iiMultimodalAgent.permission_requests_wire PROPERTIES TIMEOUT 180)
            add_test(NAME iiMultimodalAgent.control_capacity_wire COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/permission_requests_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                --control-capacity --report ${_test_output}/control-capacity-wire.json)
            set_tests_properties(iiMultimodalAgent.control_capacity_wire PROPERTIES TIMEOUT 180)
            add_test(NAME iiMultimodalAgent.working_directories_wire COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/permission_settings_wire.py $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> $<TARGET_FILE:iiMultimodalAgentMcpServer>
                --additional-directories --report ${_test_output}/working-directories-wire.json)
            set_tests_properties(iiMultimodalAgent.working_directories_wire PROPERTIES TIMEOUT 180)
        endif()
        if(IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_AGENT_CATALOG)
            add_executable(iiMultimodalAgentSubagentRuntimeSmoke tests/subagent_runtime_smoke.cpp)
            target_link_libraries(iiMultimodalAgentSubagentRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
            add_test(NAME iiMultimodalAgent.subagents_inference COMMAND iiMultimodalAgentSubagentRuntimeSmoke "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
            set_tests_properties(iiMultimodalAgent.subagents_inference PROPERTIES TIMEOUT 600 LABELS "inference")
            add_test(NAME iiMultimodalAgent.agent_profiles_api_inference COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_profile_wire.py" "$<TARGET_FILE:iiMultimodalAgentD>" "$<TARGET_FILE:iimagent>"
                "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}"
                --report "${_test_output}/agent-profiles-api-inference.json")
            set_tests_properties(iiMultimodalAgent.agent_profiles_api_inference PROPERTIES TIMEOUT 600 LABELS "inference")
            add_test(NAME iiMultimodalAgent.skill_fork_api_inference COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_profile_wire.py" "$<TARGET_FILE:iiMultimodalAgentD>" "$<TARGET_FILE:iimagent>"
                "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" --skill-forks
                --report "${_test_output}/skill-fork-api-inference.json")
            set_tests_properties(iiMultimodalAgent.skill_fork_api_inference PROPERTIES TIMEOUT 900 LABELS "inference")
            add_test(NAME iiMultimodalAgent.skill_permissions_api_inference COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_profile_wire.py" "$<TARGET_FILE:iiMultimodalAgentD>" "$<TARGET_FILE:iimagent>"
                "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" --skill-permissions
                --report "${_test_output}/skill-permissions-api-inference.json")
            set_tests_properties(iiMultimodalAgent.skill_permissions_api_inference PROPERTIES TIMEOUT 900 LABELS "inference")
        endif()
        if(IIMULTIMODALAGENT_WITH_LLAMA AND (IIMULTIMODALAGENT_TEST_CHAT_GGUF OR IIMULTIMODALAGENT_TEST_AGENT_CATALOG))
            add_executable(iiMultimodalAgentTaskRuntimeSmoke tests/task_runtime_smoke.cpp)
            target_link_libraries(iiMultimodalAgentTaskRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
            if(IIMULTIMODALAGENT_TEST_CHAT_GGUF)
                add_test(NAME iiMultimodalAgent.tasks_inference COMMAND iiMultimodalAgentTaskRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
                set_tests_properties(iiMultimodalAgent.tasks_inference PROPERTIES TIMEOUT 180 LABELS "inference")
            endif()
            if(IIMULTIMODALAGENT_TEST_AGENT_CATALOG)
                if(NOT IIMULTIMODALAGENT_TEST_AGENT_MODEL MATCHES "^model://")
                    message(FATAL_ERROR "IIMULTIMODALAGENT_TEST_AGENT_CATALOG requires IIMULTIMODALAGENT_TEST_AGENT_MODEL=model://...")
                endif()
                set(_task_profile_args)
                if(IIMULTIMODALAGENT_TEST_AGENT_NO_THINK)
                    list(APPEND _task_profile_args --no-think)
                endif()
                if(NOT IIMULTIMODALAGENT_TEST_AGENT_TOOL_GRAMMAR)
                    list(APPEND _task_profile_args --no-tool-grammar)
                endif()
                add_test(NAME iiMultimodalAgent.tasks_catalog_inference COMMAND iiMultimodalAgentTaskRuntimeSmoke --catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" ${_task_profile_args})
                add_test(NAME iiMultimodalAgent.tasks_catalog_discovery COMMAND iiMultimodalAgentTaskRuntimeSmoke --catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" --deferred ${_task_profile_args})
                set_tests_properties(iiMultimodalAgent.tasks_catalog_inference iiMultimodalAgent.tasks_catalog_discovery PROPERTIES TIMEOUT 600 LABELS "inference")
                if(IIMULTIMODALAGENT_TEST_AGENT_THINKING_CONTROL)
                    add_test(NAME iiMultimodalAgent.tasks_catalog_thinking_control COMMAND iiMultimodalAgentTaskRuntimeSmoke --catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" --deferred --disable-thinking ${_task_profile_args})
                    set_tests_properties(iiMultimodalAgent.tasks_catalog_thinking_control PROPERTIES TIMEOUT 600 LABELS "inference")
                endif()
                if(UNIX AND NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
                    add_executable(iiMultimodalAgentInputQueueRuntimeSmoke tests/input_queue_runtime_smoke.cpp)
                    target_link_libraries(iiMultimodalAgentInputQueueRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
                    add_test(NAME iiMultimodalAgent.input_queue_inference COMMAND iiMultimodalAgentInputQueueRuntimeSmoke --catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
                    set_tests_properties(iiMultimodalAgent.input_queue_inference PROPERTIES TIMEOUT 600 LABELS "inference")
                    add_executable(iiMultimodalAgentShellRuntimeSmoke tests/shell_runtime_smoke.cpp)
                    target_link_libraries(iiMultimodalAgentShellRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
                    add_test(NAME iiMultimodalAgent.shell_inference COMMAND iiMultimodalAgentShellRuntimeSmoke --catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" ${_task_profile_args})
                    set_tests_properties(iiMultimodalAgent.shell_inference PROPERTIES TIMEOUT 600 LABELS "inference")
                endif()
            endif()
        endif()
        if(IIMULTIMODALAGENT_WITH_LLAMA AND (IIMULTIMODALAGENT_TEST_CHAT_GGUF OR IIMULTIMODALAGENT_TEST_AGENT_CATALOG))
            add_executable(iiMultimodalAgentSkillRuntimeSmoke tests/skill_runtime_smoke.cpp)
            target_link_libraries(iiMultimodalAgentSkillRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
            if(IIMULTIMODALAGENT_TEST_CHAT_GGUF)
                add_test(NAME iiMultimodalAgent.skills_inference COMMAND iiMultimodalAgentSkillRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
                set_tests_properties(iiMultimodalAgent.skills_inference PROPERTIES TIMEOUT 240 LABELS "inference")
            endif()
            if(IIMULTIMODALAGENT_TEST_AGENT_CATALOG)
                add_test(NAME iiMultimodalAgent.skills_catalog_inference COMMAND iiMultimodalAgentSkillRuntimeSmoke --catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}")
                set_tests_properties(iiMultimodalAgent.skills_catalog_inference PROPERTIES TIMEOUT 600 LABELS "inference")
            endif()
        endif()
        if(IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_CHAT_GGUF)
            add_test(NAME iiMultimodalAgent.agent_api_inference COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_api_smoke.py
                $<TARGET_FILE:iiMultimodalAgentD> $<TARGET_FILE:iimagent> --model "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}"
                --report "${_test_output}/agent-api-inference-result.json")
            set_tests_properties(iiMultimodalAgent.agent_api_inference PROPERTIES TIMEOUT 240 LABELS "inference")
            add_executable(iiMultimodalAgentCompactionRuntimeSmoke tests/compaction_runtime_smoke.cpp)
            target_link_libraries(iiMultimodalAgentCompactionRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
            add_test(NAME iiMultimodalAgent.compaction_inference COMMAND iiMultimodalAgentCompactionRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
            set_tests_properties(iiMultimodalAgent.compaction_inference PROPERTIES TIMEOUT 240 LABELS "inference")
            add_executable(iiMultimodalAgentAgentRuntimeSmoke tests/agent_runtime_smoke.cpp)
            target_link_libraries(iiMultimodalAgentAgentRuntimeSmoke PRIVATE iiMultimodalAgent::iiMultimodalAgent)
            add_test(NAME iiMultimodalAgent.agent_local_inference COMMAND iiMultimodalAgentAgentRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
            set_tests_properties(iiMultimodalAgent.agent_local_inference PROPERTIES TIMEOUT 180 LABELS "inference")
            add_executable(iiMultimodalAgentProcedureRuntimeSmoke tests/procedure_runtime_smoke.cpp)
            add_executable(iiMultimodalAgentDecisionRuntimeSmoke tests/decision_runtime_smoke.cpp)
            target_link_libraries(iiMultimodalAgentDecisionRuntimeSmoke PRIVATE iiMultimodalAgent)
            add_test(NAME iiMultimodalAgent.decision_inference COMMAND iiMultimodalAgentDecisionRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
            set_tests_properties(iiMultimodalAgent.decision_inference PROPERTIES TIMEOUT 180 LABELS "inference")
            target_link_libraries(iiMultimodalAgentProcedureRuntimeSmoke PRIVATE iiMultimodalAgent)
            add_test(NAME iiMultimodalAgent.procedures_inference COMMAND iiMultimodalAgentProcedureRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}")
            set_tests_properties(iiMultimodalAgent.procedures_inference PROPERTIES TIMEOUT 180 LABELS "inference")
            if(IIMULTIMODALAGENT_TEST_MCP_PYTHON)
                add_test(NAME iiMultimodalAgent.configured_mcp_inference COMMAND iiMultimodalAgentAgentRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}"
                    "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py" --configured-eager)
                set_tests_properties(iiMultimodalAgent.configured_mcp_inference PROPERTIES TIMEOUT 180 LABELS "inference")
                if(UNIX AND NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
                    add_test(NAME iiMultimodalAgent.mcp_output_hook_inference COMMAND iiMultimodalAgentAgentRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}"
                        "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py" --mcp-output-hook)
                    set_tests_properties(iiMultimodalAgent.mcp_output_hook_inference PROPERTIES TIMEOUT 180 LABELS "inference")
                endif()
                add_test(NAME iiMultimodalAgent.discovery_inference COMMAND iiMultimodalAgentAgentRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}"
                    "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py" --discovery)
                set_tests_properties(iiMultimodalAgent.discovery_inference PROPERTIES TIMEOUT 180 LABELS "inference")
                if(IIMULTIMODALAGENT_TEST_AGENT_CATALOG AND IIMULTIMODALAGENT_TEST_AGENT_THINKING_CONTROL)
                    add_test(NAME iiMultimodalAgent.discovery_catalog_thinking_control COMMAND iiMultimodalAgentAgentRuntimeSmoke --catalog
                        "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py" --thinking-control)
                    set_tests_properties(iiMultimodalAgent.discovery_catalog_thinking_control PROPERTIES TIMEOUT 600 LABELS "inference")
                endif()
                add_test(NAME iiMultimodalAgent.agent_mcp_inference COMMAND iiMultimodalAgentAgentRuntimeSmoke "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}"
                    "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_official_server.py")
                set_tests_properties(iiMultimodalAgent.agent_mcp_inference PROPERTIES TIMEOUT 180 LABELS "inference")
                add_test(NAME iiMultimodalAgent.mcp_server_inference COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --native "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}" --report "${_test_output}/mcp-server-native-result.json")
                set_tests_properties(iiMultimodalAgent.mcp_server_inference PROPERTIES TIMEOUT 240 LABELS "inference")
                add_test(NAME iiMultimodalAgent.mcp_http_server_inference COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>
                    --http --native "${IIMULTIMODALAGENT_TEST_CHAT_GGUF}" --report "${_test_output}/mcp-http-server-native-result.json")
                set_tests_properties(iiMultimodalAgent.mcp_http_server_inference PROPERTIES TIMEOUT 240 LABELS "inference")
            endif()
        endif()
    endif()
    if(IIMULTIMODALAGENT_WITH_LLAMA AND IIMULTIMODALAGENT_TEST_MCP_PYTHON AND IIMULTIMODALAGENT_TEST_AGENT_CATALOG AND TARGET iiMultimodalAgentMcpServer)
        foreach(_transport stdio http)
            set(_permission_http)
            if(_transport STREQUAL "http")
                set(_permission_http --http)
            endif()
            add_test(NAME iiMultimodalAgent.skill_permissions_mcp_${_transport}_inference COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_server_smoke.py" $<TARGET_FILE:iiMultimodalAgentMcpServer>
                ${_permission_http} --native-catalog "${IIMULTIMODALAGENT_TEST_AGENT_CATALOG}" --native-model "${IIMULTIMODALAGENT_TEST_AGENT_MODEL}" --skill-permissions
                --report "${_test_output}/skill-permissions-mcp-${_transport}-inference.json")
            set_tests_properties(iiMultimodalAgent.skill_permissions_mcp_${_transport}_inference PROPERTIES TIMEOUT 900 LABELS "inference")
        endforeach()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_test(NAME iiMultimodalAgent.permission_symbols COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/permission_symbols.py "${CMAKE_NM}" "$<TARGET_FILE:iiMultimodalAgent>")
    endif()
    get_property(_iilocal_tests DIRECTORY PROPERTY TESTS)
    add_test(NAME iiMultimodalAgent.test_output_guard COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${iiFileProviderNative_PYTHON_DIRECTORY}" "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_output_fixture.py" "${CMAKE_COMMAND}")
    add_test(NAME iiMultimodalAgent.dependency_reuse COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${iiFileProviderNative_PYTHON_DIRECTORY}" "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dependency_reuse.py" "${CMAKE_COMMAND}")
    add_test(NAME iiMultimodalAgent.test_output_setup COMMAND "${CMAKE_COMMAND}"
        "-DRESOURCE_ROOT=${_test_state}" "-DOUTPUT_DIR=${_test_output}"
        -DACTION=setup -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_output_fixture.cmake")
    add_test(NAME iiMultimodalAgent.test_output_cleanup COMMAND "${CMAKE_COMMAND}"
        "-DRESOURCE_ROOT=${_test_state}" "-DOUTPUT_DIR=${_test_output}"
        -DACTION=cleanup -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_output_fixture.cmake")
    set_tests_properties(iiMultimodalAgent.test_output_setup PROPERTIES FIXTURES_SETUP AgentTestOutput)
    set_tests_properties(iiMultimodalAgent.test_output_cleanup PROPERTIES FIXTURES_CLEANUP AgentTestOutput)
    set_tests_properties(${_iilocal_tests} PROPERTIES FIXTURES_REQUIRED AgentTestOutput)
    set_tests_properties(${_iilocal_tests} PROPERTIES ENVIRONMENT_MODIFICATION
        "TMPDIR=set:${_test_output}/tmp;TMP=set:${_test_output}/tmp;TEMP=set:${_test_output}/tmp;IILOCALLLM_APP_ENDPOINTS=set:${_test_output}/tmp/app-endpoints")
    set_property(TEST iiMultimodalAgent.graphql APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "TMPDIR=set:${_test_output}/tmp" "TMP=set:${_test_output}/tmp" "TEMP=set:${_test_output}/tmp")
    if(APPLE)
        # A login-shell SDK override must not replace the selected package's dylibs.
        set_property(TEST ${_iilocal_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "DYLD_LIBRARY_PATH=unset:" "DYLD_FRAMEWORK_PATH=unset:"
            "DYLD_FALLBACK_LIBRARY_PATH=unset:" "DYLD_VERSIONED_LIBRARY_PATH=unset:"
            "DYLD_VERSIONED_FRAMEWORK_PATH=unset:")
    endif()
endif()
