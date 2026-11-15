if(NOT SOURCE_ROOT OR NOT BUILD_DIR OR NOT TEST_ROOT OR NOT PYTHON OR NOT CORE_DIR OR NOT CORE_PACKAGE_DIR OR NOT GENERATOR OR NOT INSTALL_LIBDIR OR NOT INSTALL_DATADIR OR NOT CTEST_EXECUTABLE)
    message(FATAL_ERROR "Missing installed question presentation consumer arguments")
endif()
set(prefix "${TEST_ROOT}/prefix")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" --config "${BUILD_CONFIG}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot stage the agent package")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DYLD_LIBRARY_PATH --unset=DYLD_FALLBACK_LIBRARY_PATH
    --unset=DYLD_FRAMEWORK_PATH --unset=DYLD_FALLBACK_FRAMEWORK_PATH --unset=IIQUESTION_PRESENTATION_LIBRARY
    "PYTHONDONTWRITEBYTECODE=1" "IIQUESTION_TEST_PYTHON_MODULE=${prefix}/${INSTALL_DATADIR}/iiMultimodalAgent/python/iiquestion_presentation.py"
    "${PYTHON}" -B "${SOURCE_ROOT}/tests/test_question_presentation_python.py" - "${CORE_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed Python question consumer failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_ROOT}/tests/question-c-consumer" -B "${TEST_ROOT}/c-consumer"
    -G "${GENERATOR}" "-DiiMultimodalAgent_DIR=${prefix}/${INSTALL_LIBDIR}/cmake/iiMultimodalAgent"
    "-DiiSDKCore_DIR=${CORE_PACKAGE_DIR}" "-DQt6_DIR=${QT_PACKAGE_DIR}" "-DiiLocalLLM_DIR=${LOCAL_LLM_DIR}"
    "-DiiDecision_DIR=${DECISION_DIR}" "-DiiLocalDiffusion_DIR=${LOCAL_DIFFUSION_DIR}" "-DiiAudioGen_DIR=${AUDIO_DIR}"
    "-DiiVoiceOver_DIR=${VOICE_DIR}" "-DiiFileProvider_DIR=${FILE_PROVIDER_DIR}"
    "-DiiFileProviderNative_DIR=${FILE_PROVIDER_NATIVE_DIR}" "-DCMAKE_BUILD_TYPE=${BUILD_CONFIG}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot configure installed C question consumer")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_ROOT}/c-consumer" --config "${BUILD_CONFIG}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot build installed C question consumer")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DYLD_LIBRARY_PATH --unset=DYLD_FALLBACK_LIBRARY_PATH
    --unset=DYLD_FRAMEWORK_PATH --unset=DYLD_FALLBACK_FRAMEWORK_PATH
    "${CTEST_EXECUTABLE}" --test-dir "${TEST_ROOT}/c-consumer" -C "${BUILD_CONFIG}" --output-on-failure RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed C question consumer failed")
endif()
