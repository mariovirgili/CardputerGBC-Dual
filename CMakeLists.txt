cmake_minimum_required(VERSION 3.16)

set(M5GFX_COMPONENT_CANDIDATES
    "${CMAKE_CURRENT_LIST_DIR}/components/M5GFX"
    "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps/cardputer_adv_idf/M5GFX"
    "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps/m5stack-stamps3/M5GFX"
)
set(M5UNIFIED_COMPONENT_CANDIDATES
    "${CMAKE_CURRENT_LIST_DIR}/components/M5Unified"
    "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps/cardputer_adv_idf/M5Unified"
    "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps/m5stack-stamps3/M5Unified"
)

foreach(component_dir ${M5GFX_COMPONENT_CANDIDATES})
    if(NOT M5GFX_COMPONENT_DIR AND EXISTS "${component_dir}/CMakeLists.txt")
        set(M5GFX_COMPONENT_DIR "${component_dir}")
    endif()
endforeach()

foreach(component_dir ${M5UNIFIED_COMPONENT_CANDIDATES})
    if(NOT M5UNIFIED_COMPONENT_DIR AND EXISTS "${component_dir}/CMakeLists.txt")
        set(M5UNIFIED_COMPONENT_DIR "${component_dir}")
    endif()
endforeach()

list(APPEND EXTRA_COMPONENT_DIRS "${M5GFX_COMPONENT_DIR}" "${M5UNIFIED_COMPONENT_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(CardputerGBCDual)
