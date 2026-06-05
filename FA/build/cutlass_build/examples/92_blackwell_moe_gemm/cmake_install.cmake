# Install script for directory: /home/ubuntu/workspace/oyhj/cutlass/examples/92_blackwell_moe_gemm

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "install")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_regular" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_regular")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_regular"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/92_blackwell_moe_gemm_regular")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_regular" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_regular")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_regular")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_92_blackwell_moe_gemm_regular" TYPE FILE RENAME "CTestTestfile.ctest_examples_92_blackwell_moe_gemm_regular.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/ctest/ctest_examples_92_blackwell_moe_gemm_regular/CTestTestfile.ctest_examples_92_blackwell_moe_gemm_regular.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_grouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_grouped")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_grouped"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/92_blackwell_moe_gemm_grouped")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_grouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_grouped")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_grouped")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_92_blackwell_moe_gemm_grouped" TYPE FILE RENAME "CTestTestfile.ctest_examples_92_blackwell_moe_gemm_grouped.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/ctest/ctest_examples_92_blackwell_moe_gemm_grouped/CTestTestfile.ctest_examples_92_blackwell_moe_gemm_grouped.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_rcgrouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_rcgrouped")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_rcgrouped"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/92_blackwell_moe_gemm_rcgrouped")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_rcgrouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_rcgrouped")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_rcgrouped")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_92_blackwell_moe_gemm_rcgrouped" TYPE FILE RENAME "CTestTestfile.ctest_examples_92_blackwell_moe_gemm_rcgrouped.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/ctest/ctest_examples_92_blackwell_moe_gemm_rcgrouped/CTestTestfile.ctest_examples_92_blackwell_moe_gemm_rcgrouped.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_blockscaled_rcgrouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_blockscaled_rcgrouped")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_blockscaled_rcgrouped"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/92_blackwell_moe_gemm_blockscaled_rcgrouped")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_blockscaled_rcgrouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_blockscaled_rcgrouped")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_blockscaled_rcgrouped")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_92_blackwell_moe_gemm_blockscaled_rcgrouped" TYPE FILE RENAME "CTestTestfile.ctest_examples_92_blackwell_moe_gemm_blockscaled_rcgrouped.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/ctest/ctest_examples_92_blackwell_moe_gemm_blockscaled_rcgrouped/CTestTestfile.ctest_examples_92_blackwell_moe_gemm_blockscaled_rcgrouped.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_regular" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_regular")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_regular"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/92_blackwell_moe_gemm_fp4_regular")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_regular" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_regular")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_regular")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_92_blackwell_moe_gemm_fp4_regular" TYPE FILE RENAME "CTestTestfile.ctest_examples_92_blackwell_moe_gemm_fp4_regular.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/ctest/ctest_examples_92_blackwell_moe_gemm_fp4_regular/CTestTestfile.ctest_examples_92_blackwell_moe_gemm_fp4_regular.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_grouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_grouped")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_grouped"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/92_blackwell_moe_gemm_fp4_grouped")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_grouped" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_grouped")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/92_blackwell_moe_gemm_fp4_grouped")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_92_blackwell_moe_gemm_fp4_grouped" TYPE FILE RENAME "CTestTestfile.ctest_examples_92_blackwell_moe_gemm_fp4_grouped.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/92_blackwell_moe_gemm/ctest/ctest_examples_92_blackwell_moe_gemm_fp4_grouped/CTestTestfile.ctest_examples_92_blackwell_moe_gemm_fp4_grouped.install.cmake")
endif()

