add_executable(atomicripper-tests
    test_toc.cpp
    test_accuraterip.cpp
    test_cuesh.cpp
    test_flacenc.cpp
)

target_link_libraries(atomicripper-tests PRIVATE
    atomicripper-core
    Catch2::Catch2WithMain
)

target_include_directories(atomicripper-tests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME atomicripper-tests COMMAND atomicripper-tests)
