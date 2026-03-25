set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(atomicripper-gui WIN32
    main.cpp
    MainWindow.hpp
    MainWindow.cpp
    ReleaseDialog.hpp
    ReleaseDialog.cpp
    TrackTableWidget.hpp
    TrackTableWidget.cpp
)

target_link_libraries(atomicripper-gui PRIVATE
    atomicripper-core
    Qt6::Widgets
    Qt6::Network
    Qt6::Core
)

target_include_directories(atomicripper-gui PRIVATE
    ${CMAKE_SOURCE_DIR}
)

set_target_properties(atomicripper-gui PROPERTIES
    OUTPUT_NAME "AtomicRipper-gui"
)

# Deploy Qt DLLs next to the executable after each build (Windows only).
# windeployqt copies Qt runtimes, plugins, and translations automatically.
if(WIN32)
    find_program(WINDEPLOYQT windeployqt
        HINTS "${Qt6_DIR}/../../../bin"
        NO_DEFAULT_PATH)
    if(WINDEPLOYQT)
        add_custom_command(TARGET atomicripper-gui POST_BUILD
            COMMAND "${WINDEPLOYQT}"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:atomicripper-gui>"
            COMMENT "Deploying Qt runtime…"
            VERBATIM)
    endif()
endif()
