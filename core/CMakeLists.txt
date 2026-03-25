add_library(atomicripper-core STATIC
    drive/TOC.cpp
    drive/Drive.cpp
    drive/DriveEnumerator.cpp
    metadata/DiscId.cpp
    rip/RipEngine.cpp
)

target_include_directories(atomicripper-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(atomicripper-core PUBLIC cxx_std_20)

# Windows: link bcrypt for SHA-1 (used in DiscId) and ntddcdrm for CD IOCTLs
if(WIN32)
    target_link_libraries(atomicripper-core PRIVATE bcrypt)
endif()

# Future phases will add FLAC, TagLib, cpr here as needed:
# target_link_libraries(atomicripper-core PUBLIC FLAC::FLAC TagLib::tag cpr::cpr)
