# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)
project(hellovulkantexture LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

if(NOT DEFINED INSTALL_EXAMPLESDIR)
    set(INSTALL_EXAMPLESDIR "examples")
endif()

set(INSTALL_EXAMPLEDIR "${INSTALL_EXAMPLESDIR}/vulkan/hellovulkantexture")

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

qt_add_executable(hellovulkantexture
    hellovulkantexture.cpp hellovulkantexture.h
    main.cpp
)

set_target_properties(hellovulkantexture PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(hellovulkantexture PUBLIC
    Qt::Core
    Qt::Gui
)

# Resources:
set(hellovulkantexture_resource_files
    "qt256.png"
    "texture_frag.spv"
    "texture_vert.spv"
)

qt_add_resources(hellovulkantexture "hellovulkantexture"
    PREFIX
        "/"
    FILES
        ${hellovulkantexture_resource_files}
)

install(TARGETS hellovulkantexture
    RUNTIME DESTINATION "${INSTALL_EXAMPLEDIR}"
    BUNDLE DESTINATION "${INSTALL_EXAMPLEDIR}"
    LIBRARY DESTINATION "${INSTALL_EXAMPLEDIR}"
)
