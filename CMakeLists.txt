cmake_minimum_required(VERSION 3.3)
project (cycle_ptr)
enable_testing()

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

set(CYCLE_PTR_VERSION_MAJOR 0 CACHE STRING "major version" FORCE)
set(CYCLE_PTR_VERSION_MINOR 0 CACHE STRING "minor version" FORCE)
set(CYCLE_PTR_VERSION ${CYCLE_PTR_VERSION_MAJOR}.${CYCLE_PTR_VERSION_MINOR} CACHE STRING "version" FORCE)

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

# Set up flags for the C++ feature test macro.
if(STD_CXX17)
  set (CMAKE_REQUIRED_FLAGS "-std=c++17")
elseif(STD_CXX1Z)
  set (CMAKE_REQUIRED_FLAGS "-std=c++1z")
endif()

include(CheckCXXSymbolExists)
check_cxx_symbol_exists(std::hardware_destructive_interference_size "new" HAS_HARDWARE_INTERFERENCE_SIZE)
mark_as_advanced(HAS_HARDWARE_INTERFERENCE_SIZE)

# Unset the flags.
# Otherwise the thread-check below may fail (because it is a C-language test,
# while our options are C++ options).
unset(CMAKE_REQUIRED_FLAGS)

set(headers
    include/cycle_ptr/allocator.h
    include/cycle_ptr/cycle_ptr.h
    include/cycle_ptr/util.h
    )
set(headers_detail
    include/cycle_ptr/detail/base_control.h
    include/cycle_ptr/detail/color.h
    include/cycle_ptr/detail/control.h
    include/cycle_ptr/detail/generation.h
    include/cycle_ptr/detail/hazard.h
    include/cycle_ptr/detail/intrusive_ptr.h
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

find_package (Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
  target_link_libraries(cycle_ptr PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

target_include_directories(cycle_ptr PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

configure_file(workarounds.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/cycle_ptr/detail/workarounds.h @ONLY)
target_include_directories(cycle_ptr PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/cycle_ptr/detail/workarounds.h DESTINATION "include/cycle_ptr/detail")

install(FILES ${headers} DESTINATION "include/cycle_ptr")
install(FILES ${headers_detail} DESTINATION "include/cycle_ptr/detail")
install(TARGETS cycle_ptr EXPORT cycle_ptr DESTINATION "lib")
install(EXPORT cycle_ptr DESTINATION "lib/cmake/cycle_ptr")

configure_file(cycle_ptr-config-version.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cycle_ptr-config-version.cmake @ONLY)
install(FILES cycle_ptr-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/cycle_ptr-config-version.cmake DESTINATION "lib/cmake/cycle_ptr")

add_subdirectory (test)

find_package(Doxygen COMPONENTS mscgen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "README.md")
  doxygen_add_docs(cycle_ptr-doc ${DOXYGEN_USE_MDFILE_AS_MAINPAGE} include doxygen)
endif()
