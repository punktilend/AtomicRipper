add_executable(atomicripper-cli main.cpp)

target_link_libraries(atomicripper-cli PRIVATE atomicripper-core)

# Make #include <core/...> work
target_include_directories(atomicripper-cli PRIVATE
    ${CMAKE_SOURCE_DIR}
)

set_target_properties(atomicripper-cli PROPERTIES
    OUTPUT_NAME "atomicripper"
)
