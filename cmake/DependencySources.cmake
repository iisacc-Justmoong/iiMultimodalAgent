# Reuse an existing dependency source tree instead of letting FetchContent
# replace it. An incomplete tree is an error; a missing tree may be downloaded.
function(ii_agent_reuse_dependency variable source_root directory sentinel)
    if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
        return()
    endif()
    set(path "${source_root}/resources/dependencies/${directory}")
    if(EXISTS "${path}")
        if(NOT EXISTS "${path}/${sentinel}")
            message(FATAL_ERROR "Incomplete dependency source at ${path}; preserve or repair it before configuring")
        endif()
        set(${variable} "${path}" PARENT_SCOPE)
    endif()
endfunction()
