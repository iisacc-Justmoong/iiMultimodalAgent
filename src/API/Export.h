#pragma once
#if defined(_WIN32)
# if defined(IIMULTIMODALAGENT_BUILDING_LIBRARY)
#  define IIMULTIMODALAGENT_EXPORT __declspec(dllexport)
# else
#  define IIMULTIMODALAGENT_EXPORT __declspec(dllimport)
# endif
#else
# define IIMULTIMODALAGENT_EXPORT __attribute__((visibility("default")))
#endif
