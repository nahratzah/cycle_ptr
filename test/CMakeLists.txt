find_package(UnitTest++)

if (UnitTest++_FOUND)
  add_executable (cycle_ptr_tests test.cc gptr.cc member_ptr.cc)
  target_link_libraries (cycle_ptr_tests cycle_ptr)
  target_link_libraries (cycle_ptr_tests UnitTest++)
  target_include_directories (cycle_ptr_tests PUBLIC ${UTPP_INCLUDE_DIRS})
  target_compile_features (cycle_ptr_tests PUBLIC cxx_std_17)
  set_target_properties (cycle_ptr_tests PROPERTIES CXX_EXTENSIONS OFF)

  add_test (NAME cycle_ptr COMMAND $<TARGET_FILE:cycle_ptr_tests>)
endif ()
