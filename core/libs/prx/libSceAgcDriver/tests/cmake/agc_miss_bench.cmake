add_executable(agc_miss_bench EXCLUDE_FROM_ALL
        tools/MissBench.cpp
        Execution/src/ShaderMemory.cpp
        Graphics/src/RuntimeSwitches.cpp
        Execution/src/GuestMemory.cpp
        Execution/src/PerformanceTraceThread.cpp
)
target_include_directories(agc_miss_bench PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_miss_bench PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_miss_bench PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_miss_bench)
