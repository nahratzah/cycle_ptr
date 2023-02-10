cmake_minimum_required(VERSION 3.8)
project (cycle_ptr)
enable_testing()

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

set(CYCLE_PTR_VERSION_MAJOR 0 CACHE STRING "major version" FORCE)
set(CYCLE_PTR_VERSION_MINOR 5 CACHE STRING "minor version" FORCE)
set(CYCLE_PTR_VERSION ${CYCLE_PTR_VERSION_MAJOR}.${CYCLE_PTR_VERSION_MINOR} CACHE STRING "version" FORCE)

# On windows, DLLs must be either in the search path, or in the same directory as the executable.
# Since our test binaries are in a subdirectory, our tests fail under windows because they can't load the DLL.
# In order to sidestep that, we use the CMAKE_RUNTIME_OUTPUT_DIRECTORY to get all executables into the same directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

find_program(CLANG_TIDY_BINARY clang-tidy14)
if (CLANG_TIDY_BINARY)
  set(CMAKE_CXX_CLANG_TIDY
    ${CLANG_TIDY_BINARY};
    --format-style='file';
    --header-filter=${CMAKE_CURRENT_SOURCE_DIR};
    --use-color;
  )
endif ()

include(CheckCXXSymbolExists)
check_cxx_symbol_exists(std::hardware_destructive_interference_size "new" HAS_HARDWARE_INTERFERENCE_SIZE)
mark_as_advanced(HAS_HARDWARE_INTERFERENCE_SIZE)

set(headers
    include/cycle_ptr.h
    )

add_library (cycle_ptr INTERFACE)
set_property (TARGET cycle_ptr PROPERTY VERSION ${CYCLE_PTR_VERSION})
target_compile_features (cycle_ptr INTERFACE cxx_std_17)
set_target_properties (cycle_ptr PROPERTIES CXX_EXTENSIONS OFF)

find_package (Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
  target_link_libraries(cycle_ptr INTERFACE ${CMAKE_THREAD_LIBS_INIT})
endif()

target_include_directories(cycle_ptr INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

install(FILES ${headers} DESTINATION "include")

configure_file(cycle_ptr-config-version.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cycle_ptr-config-version.cmake @ONLY)
install(FILES cycle_ptr-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/cycle_ptr-config-version.cmake DESTINATION "lib/cmake/cycle_ptr")

add_subdirectory (test)

find_package(Doxygen COMPONENTS mscgen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "README.md")
  doxygen_add_docs(cycle_ptr-doc ${DOXYGEN_USE_MDFILE_AS_MAINPAGE} include doxygen)
endif()
