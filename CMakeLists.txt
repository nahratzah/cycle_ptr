cmake_minimum_required(VERSION 3.3)
project (cycle_ptr)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

set(CYCLE_PTR_VERSION_MAJOR 0 CACHE STRING "major version" FORCE)
set(CYCLE_PTR_VERSION_MINOR 0 CACHE STRING "minor version" FORCE)
set(CYCLE_PTR_VERSION ${CYCLE_PTR_VERSION_MAJOR}.${CYCLE_PTR_VERSION_MINOR} CACHE STRING "version" FORCE)

enable_testing()

include (CheckCXXCompilerFlag)
check_cxx_compiler_flag ("-std=c++17" STD_CXX17)
check_cxx_compiler_flag ("-std=c++1z" STD_CXX1Z)
if(STD_CXX17)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
elseif(STD_CXX1Z)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++1z")
else()
  message(WARNING "Cannot figure out how to enable C++17 on compiler.")
endif()

set(headers
    include/cycle_ptr/cycle_ptr.h
    )
set(headers_detail
    include/cycle_ptr/detail/base_control.h
    include/cycle_ptr/detail/color.h
    include/cycle_ptr/detail/control.h
    include/cycle_ptr/detail/generation.h
    include/cycle_ptr/detail/hazard.h
    include/cycle_ptr/detail/llist.h
    include/cycle_ptr/detail/vertex.h
    )

add_library (cycle_ptr
    src/vertex.cc
    src/base_control.cc
    src/generation.cc
    )
set_property (TARGET cycle_ptr PROPERTY VERSION ${CYCLE_PTR_VERSION})
set_property (TARGET cycle_ptr PROPERTY SOVERSION "0.0")

find_package (Boost REQUIRED)
find_package (Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
  target_link_libraries(cycle_ptr PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

target_include_directories(cycle_ptr PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${Boost_INCLUDE_DIR})

install(FILES ${headers} DESTINATION "include/cycle_ptr")
install(FILES ${headers_detail} DESTINATION "include/cycle_ptr/detail")
install(TARGETS cycle_ptr EXPORT cycle_ptr DESTINATION "lib")
install(EXPORT cycle_ptr DESTINATION "lib/cmake/cycle_ptr")

configure_file(cycle_ptr-config-version.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cycle_ptr-config-version.cmake @ONLY)
install(FILES cycle_ptr-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/cycle_ptr-config-version.cmake DESTINATION "lib/cmake/cycle_ptr")

# add_subdirectory (test)

find_package(Doxygen REQUIRED mscgen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "README.md")
  doxygen_add_docs(cycle_ptr-doc ${DOXYGEN_USE_MDFILE_AS_MAINPAGE} include doxygen)
endif()
