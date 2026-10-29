if(IS_DIRECTORY "${SOURCE_ROOT}/src/iiMultimodalAgent")
    message(FATAL_ERROR "The source directory repeats the project name")
endif()
foreach(_file API/Inference.h Agent/Runtime/Engine.h Agent/Runtime/Engine.cpp
        Agent/Memory/Rag.h Agent/Memory/Rag.cpp Agent/Permissions/DecisionGate.h
        Agent/Permissions/DecisionGate.cpp Agent/Tools/MultimodalTools.h
        Agent/Tools/MultimodalTools.cpp MCP/Client.h MCP/Client.cpp Diagnostics/FailureReport.h)
    if(NOT EXISTS "${SOURCE_ROOT}/src/${_file}")
        message(FATAL_ERROR "Missing responsibility-grouped source: ${_file}")
    endif()
endforeach()
