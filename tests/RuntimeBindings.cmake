add_executable(iiMultimodalAgentRuntimeBindingsTests "${CMAKE_CURRENT_LIST_DIR}/runtime_bindings.cpp")
target_link_libraries(iiMultimodalAgentRuntimeBindingsTests PRIVATE
    iiMultimodalAgent::iiMultimodalAgent iiFileProvider::Native iiLocalDiffusion::iiLocalDiffusion ${CMAKE_DL_LIBS})
add_test(NAME iiMultimodalAgent.runtime_bindings COMMAND iiMultimodalAgentRuntimeBindingsTests
    "$<TARGET_FILE:iiFileProvider::Native>" "$<TARGET_FILE:iiLocalLLM::iiLocalLLM>"
    "$<TARGET_FILE:iiLocalDiffusion::iiLocalDiffusion>" "$<TARGET_FILE:iiAudioGen::iiAudioGen>"
    "$<TARGET_FILE:iiVoiceOver::iiVoiceOver>" "$<TARGET_FILE:iiMultimodalAgent::iiMultimodalAgent>")
set_tests_properties(iiMultimodalAgent.runtime_bindings PROPERTIES TIMEOUT 600)
add_executable(iiMultimodalAgentRuntimeBootstrap "${CMAKE_CURRENT_LIST_DIR}/runtime_bootstrap.cpp")
target_link_libraries(iiMultimodalAgentRuntimeBootstrap PRIVATE iiMultimodalAgent::iiMultimodalAgent ${CMAKE_DL_LIBS})
add_test(NAME iiMultimodalAgent.runtime_bootstrap COMMAND iiMultimodalAgentRuntimeBootstrap
    "$<TARGET_FILE:iiLocalDiffusion::iiLocalDiffusion>")
set_tests_properties(iiMultimodalAgent.runtime_bootstrap PROPERTIES TIMEOUT 120)
