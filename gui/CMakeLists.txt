# GUI target — only built when Qt6 is available
# Phase 6: implement after CLI+core is solid

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(atomicripper-gui WIN32
    main.cpp
)

target_link_libraries(atomicripper-gui PRIVATE
    atomicripper-core
    Qt6::Widgets
    Qt6::Network
)

target_include_directories(atomicripper-gui PRIVATE
    ${CMAKE_SOURCE_DIR}
)
