cmake_minimum_required(VERSION 3.24)
project(iiMultimodalAgent VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

add_library(iiMultimodalAgentFailureDiagnostics INTERFACE)
add_library(iiMultimodalAgent::FailureDiagnostics ALIAS iiMultimodalAgentFailureDiagnostics)
set_target_properties(iiMultimodalAgentFailureDiagnostics PROPERTIES EXPORT_NAME FailureDiagnostics)
target_compile_features(iiMultimodalAgentFailureDiagnostics INTERFACE cxx_std_23)
target_include_directories(iiMultimodalAgentFailureDiagnostics INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src> $<INSTALL_INTERFACE:include>)
install(TARGETS iiMultimodalAgentFailureDiagnostics EXPORT iiMultimodalAgentFailureDiagnosticsTargets COMPONENT FailureDiagnostics)
install(EXPORT iiMultimodalAgentFailureDiagnosticsTargets NAMESPACE iiMultimodalAgent::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiMultimodalAgent COMPONENT FailureDiagnostics)
install(FILES src/iiMultimodalAgent/diagnostics/FailureReport.h DESTINATION include/iiMultimodalAgent/diagnostics COMPONENT FailureDiagnostics)
if(BUILD_TESTING)
    add_executable(iiMultimodalAgentFailureReportTests tests/failure_report_tests.cpp)
    target_link_libraries(iiMultimodalAgentFailureReportTests PRIVATE iiMultimodalAgent::FailureDiagnostics)
    add_test(NAME iiMultimodalAgent.failure_report COMMAND iiMultimodalAgentFailureReportTests)
endif()

if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME})
        set(_agent_home "$ENV{HOME}")
    elseif(DEFINED ENV{USERPROFILE})
        set(_agent_home "$ENV{USERPROFILE}")
    else()
        message(FATAL_ERROR "Set CMAKE_INSTALL_PREFIX explicitly")
    endif()
    set(CMAKE_INSTALL_PREFIX "${_agent_home}/.local/SDK/iiMultimodalAgent" CACHE PATH "Install prefix" FORCE)
endif()

find_package(iiLocalLLM 0.56.1 CONFIG REQUIRED)
find_package(iiDecision 0.0.5 CONFIG REQUIRED)
find_package(iiLocalDiffusion 0.6 CONFIG REQUIRED)
find_package(iiAudioGen 0.3 CONFIG REQUIRED)
find_package(iiVoiceOver 0.2 CONFIG REQUIRED)
find_package(iiFileProvider 0.6 CONFIG REQUIRED)
find_package(iiFileProviderNative 0.8.2 CONFIG REQUIRED)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Network)
find_package(Threads REQUIRED)
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# This package owns every harness implementation and its private parsers.
include(cmake/Sources.cmake)
add_library(iiMultimodalAgentImageTypes INTERFACE)
set_target_properties(iiMultimodalAgentImageTypes PROPERTIES EXPORT_NAME ImageTypes)
target_compile_features(iiMultimodalAgentImageTypes INTERFACE cxx_std_23)
target_include_directories(iiMultimodalAgentImageTypes SYSTEM INTERFACE
    "$<TARGET_PROPERTY:iiLocalDiffusion::iiLocalDiffusion,INTERFACE_INCLUDE_DIRECTORIES>")
add_library(iiMultimodalAgentImages SHARED src/providers/ImagePlugin.cpp)
target_compile_features(iiMultimodalAgentImages PRIVATE cxx_std_23)
target_include_directories(iiMultimodalAgentImages PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(iiMultimodalAgentImages PRIVATE iiFileProvider::Native iiLocalDiffusion::iiLocalDiffusion)
if(APPLE)
    get_target_property(_native_provider_kind iiFileProvider::Native TYPE)
    if(_native_provider_kind STREQUAL "SHARED_LIBRARY")
        # CMake/macOS keep the last repeated dylib entry, so nominal library
        # ordering alone cannot protect this choice from bundled older copies.
        target_link_options(iiMultimodalAgentImages BEFORE PRIVATE
            "LINKER:-needed_library,$<TARGET_FILE:iiFileProvider::Native>")
    endif()
endif()
set_target_properties(iiMultimodalAgentImages PROPERTIES
    EXPORT_NAME NativeImages
    CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    INSTALL_RPATH_USE_LINK_PATH TRUE)
add_library(iiMultimodalAgent SHARED ${_agent_sources}
    src/iiMultimodalAgent/agent/MultimodalTools.cpp src/providers/ImageLoader.cpp)
add_dependencies(iiMultimodalAgent iiMultimodalAgentImages)
add_library(iiMultimodalAgent::iiMultimodalAgent ALIAS iiMultimodalAgent)
target_compile_features(iiMultimodalAgent PUBLIC cxx_std_23)
target_compile_definitions(iiMultimodalAgent PRIVATE IIMULTIMODALAGENT_BUILDING_LIBRARY
    IIMULTIMODALAGENT_IMAGE_PLUGIN_NAME="$<TARGET_FILE_NAME:iiMultimodalAgentImages>")
target_link_libraries(iiMultimodalAgent
    PRIVATE "$<LINK_ONLY:iiFileProvider::Native>"
    PUBLIC iiLocalLLM::iiLocalLLM iiDecision::iiDecision
        iiMultimodalAgentImageTypes iiAudioGen::iiAudioGen iiVoiceOver::iiVoiceOver
    PRIVATE iiFileProvider::iiFileProvider Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE AND _native_provider_kind STREQUAL "SHARED_LIBRARY")
    target_link_options(iiMultimodalAgent BEFORE PUBLIC
        "LINKER:-needed_library,$<TARGET_FILE:iiFileProvider::Native>")
endif()
target_include_directories(iiMultimodalAgent PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")
set_target_properties(iiMultimodalAgent PROPERTIES
    AUTOMOC ON CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    INSTALL_RPATH_USE_LINK_PATH TRUE)
if(WIN32)
    target_link_libraries(iiMultimodalAgent PRIVATE ws2_32)
endif()
include(cmake/JsonSchema.cmake)
include(cmake/ContextParsers.cmake)
include(cmake/WebParser.cmake)
include(cmake/PermissionParsers.cmake)

add_executable(iiMultimodalAgentD tools/service_main.cpp)
add_executable(iiMultimodalAgentService tools/service_main.cpp)
add_executable(iimagent tools/client_main.cpp tools/IpcClient.cpp)
add_executable(iimagent-rag tools/rag_main.cpp)
add_executable(iiMultimodalAgentPlugins tools/plugins_main.cpp)
set_target_properties(iiMultimodalAgentService PROPERTIES OUTPUT_NAME iimagent-service)
set_target_properties(iiMultimodalAgentPlugins PROPERTIES OUTPUT_NAME iimagent-plugins)
set(_agent_entrypoints iiMultimodalAgentD iiMultimodalAgentService iimagent iimagent-rag iiMultimodalAgentPlugins)
if(UNIX)
    add_executable(iiMultimodalAgentMcpServer tools/mcp_main.cpp)
    set_target_properties(iiMultimodalAgentMcpServer PROPERTIES OUTPUT_NAME iimagent-mcp)
    list(APPEND _agent_entrypoints iiMultimodalAgentMcpServer)
endif()
foreach(_entrypoint IN LISTS _agent_entrypoints)
    if(_entrypoint STREQUAL "iimagent")
        target_link_libraries(${_entrypoint} PRIVATE Qt6::Core Qt6::Network)
    else()
        target_link_libraries(${_entrypoint} PRIVATE iiMultimodalAgent::iiMultimodalAgent)
    endif()
    target_compile_definitions(${_entrypoint} PRIVATE IILOCALLLM_APP_VERSION="${PROJECT_VERSION}")
    set_target_properties(${_entrypoint} PROPERTIES CXX_EXTENSIONS OFF INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set_target_properties(${_entrypoint} PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
        set_target_properties(${_entrypoint} PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
endforeach()
set(_versioned_entrypoints ${_agent_entrypoints})

set(IIMULTIMODALAGENT_WITH_LLAMA ${iiLocalLLM_WITH_LLAMA})
include(cmake/TestOptions.cmake)
if(BUILD_TESTING AND IIMULTIMODALAGENT_TEST_MCP_PYTHON)
    add_custom_target(iiMultimodalAgentMcpDependencyArtifacts ALL
        COMMAND "${IIMULTIMODALAGENT_TEST_MCP_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_mcp_bytecode.py"
        COMMENT "Prepare managed MCP dependency bytecode" VERBATIM)
endif()
include(tests/CMakeTests.cmake)

install(TARGETS iiMultimodalAgent iiMultimodalAgentImageTypes EXPORT iiMultimodalAgentTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS iiMultimodalAgentImages EXPORT iiMultimodalAgentTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS ${_agent_entrypoints} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
include(cmake/PublicHeaders.cmake)
install(FILES src/iiMultimodalAgent.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/qml/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiMultimodalAgent/qml)
install(FILES catalog/harness-parity.json README.md LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiMultimodalAgent)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiMultimodalAgent/docs)
install(FILES third_party/cpp-httplib/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiMultimodalAgent/licenses RENAME cpp-httplib.LICENSE)
install(FILES third_party/jsoncons/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiMultimodalAgent/licenses RENAME jsoncons.LICENSE)
install(FILES third_party/jsoncons/A5HASH-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiMultimodalAgent/licenses RENAME jsoncons-A5HASH.LICENSE)
install(EXPORT iiMultimodalAgentTargets NAMESPACE iiMultimodalAgent::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiMultimodalAgent COMPONENT FailureDiagnostics)
configure_package_config_file(cmake/iiMultimodalAgentConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/iiMultimodalAgentConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiMultimodalAgent)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/iiMultimodalAgentConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/iiMultimodalAgentConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/iiMultimodalAgentConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiMultimodalAgent COMPONENT FailureDiagnostics)
