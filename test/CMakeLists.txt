include(CTest)
find_package(UnitTest++)

macro (do_test binary)
  add_executable (test_${binary} ${binary}.cc)
  target_link_libraries (test_${binary} cycle_ptr)
  target_link_libraries (test_${binary} UnitTest++)
  add_test (${binary} test_${binary})
endmacro (do_test)

if (UnitTest++_FOUND)
  include_directories(${UTPP_INCLUDE_DIRS})

  do_test (gptr)
  do_test (member_ptr)
endif ()
