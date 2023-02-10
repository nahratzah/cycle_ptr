find_package(UnitTest++)

if (UnitTest++_FOUND)
  add_executable (tests test.cc gptr.cc member_ptr.cc)
  target_link_libraries (tests cycle_ptr)
  target_link_libraries (tests UnitTest++)
  target_include_directories (tests PUBLIC ${UTPP_INCLUDE_DIRS})
  target_compile_features (tests PUBLIC cxx_std_17)
  set_target_properties (tests PROPERTIES CXX_EXTENSIONS OFF)

  add_test (NAME tests COMMAND $<TARGET_FILE:tests>)
endif ()
