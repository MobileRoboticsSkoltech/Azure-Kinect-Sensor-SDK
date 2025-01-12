# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

set(Boost_USE_STATIC_LIBS ON) 
set(Boost_USE_MULTITHREADED OFF)  
set(Boost_USE_STATIC_RUNTIME OFF) 
find_package(Boost COMPONENTS filesystem) 

include_directories(${Boost_INCLUDE_DIRS}) 

add_executable(mrob_recorder
    main.cpp 
    recorder.cpp 
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc)

target_link_libraries(mrob_recorder PRIVATE
    k4a::k4a
    k4a::k4arecord
    ${Boost_LIBRARIES}
)

# Include ${CMAKE_CURRENT_BINARY_DIR}/version.rc in the target's sources
# to embed version information
set(K4A_FILEDESCRIPTION "Azure Kinect Recording Tool")
set(K4A_ORIGINALFILENAME "mrob_recorder.exe")
configure_file(
    ${K4A_VERSION_RC}
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY
    )

# Setup install
include(GNUInstallDirs)

install(
    TARGETS
        mrob_recorder
    RUNTIME DESTINATION
        ${CMAKE_INSTALL_BINDIR}
    COMPONENT
        tools
)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    install(
        FILES
            $<TARGET_PDB_FILE:mrob_recorder>
        DESTINATION
            ${CMAKE_INSTALL_BINDIR}
        COMPONENT
            tools
        OPTIONAL
    )
endif()