find_package(FLAC         CONFIG REQUIRED)
find_package(cpr          CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(TagLib        CONFIG REQUIRED)

add_library(atomicripper-core STATIC
    drive/TOC.cpp
    drive/Drive.cpp
    drive/DriveEnumerator.cpp
    metadata/DiscId.cpp
    metadata/MusicBrainz.cpp
    metadata/TagWriter.cpp
    metadata/CoverArt.cpp
    rip/RipEngine.cpp
    encode/FlacEncoder.cpp
    encode/WavEncoder.cpp
    verify/AccurateRip.cpp
    pipeline/Pipeline.cpp
)

target_include_directories(atomicripper-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(atomicripper-core PUBLIC cxx_std_20)

target_link_libraries(atomicripper-core PUBLIC
    FLAC::FLAC
    cpr::cpr
    nlohmann_json::nlohmann_json
    TagLib::tag
)

# Windows: link bcrypt for SHA-1 (used in DiscId)
if(WIN32)
    target_link_libraries(atomicripper-core PRIVATE bcrypt)
endif()
