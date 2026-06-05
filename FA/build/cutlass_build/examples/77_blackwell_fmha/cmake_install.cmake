# Install script for directory: /home/ubuntu/workspace/oyhj/cutlass/examples/77_blackwell_fmha

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
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp8")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp8"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/1177_blackwell_fmha_fp8")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp8")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp8")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_1177_blackwell_fmha_fp8" TYPE FILE RENAME "CTestTestfile.ctest_examples_1177_blackwell_fmha_fp8.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_1177_blackwell_fmha_fp8/CTestTestfile.ctest_examples_1177_blackwell_fmha_fp8.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp8")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp8"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_fmha_gen_fp8")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp8")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp8")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_fmha_gen_fp8" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_fmha_gen_fp8.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_fmha_gen_fp8/CTestTestfile.ctest_examples_77_blackwell_fmha_gen_fp8.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp8")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp8"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_mla_2sm_fp8")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp8")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp8")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_mla_2sm_fp8" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_mla_2sm_fp8.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_mla_2sm_fp8/CTestTestfile.ctest_examples_77_blackwell_mla_2sm_fp8.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp8")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp8"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_mla_2sm_cpasync_fp8")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp8")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp8")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_mla_2sm_cpasync_fp8" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_mla_2sm_cpasync_fp8.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_mla_2sm_cpasync_fp8/CTestTestfile.ctest_examples_77_blackwell_mla_2sm_cpasync_fp8.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp8")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp8"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_fmha_bwd_fp8")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp8")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp8")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_fmha_bwd_fp8" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_fmha_bwd_fp8.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_fmha_bwd_fp8/CTestTestfile.ctest_examples_77_blackwell_fmha_bwd_fp8.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp8")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp8"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_mla_fwd_fp8")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp8" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp8")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp8")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_mla_fwd_fp8" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_mla_fwd_fp8.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_mla_fwd_fp8/CTestTestfile.ctest_examples_77_blackwell_mla_fwd_fp8.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp16")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp16"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/1177_blackwell_fmha_fp16")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp16")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/1177_blackwell_fmha_fp16")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_1177_blackwell_fmha_fp16" TYPE FILE RENAME "CTestTestfile.ctest_examples_1177_blackwell_fmha_fp16.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_1177_blackwell_fmha_fp16/CTestTestfile.ctest_examples_1177_blackwell_fmha_fp16.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp16")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp16"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_fmha_gen_fp16")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp16")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_gen_fp16")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_fmha_gen_fp16" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_fmha_gen_fp16.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_fmha_gen_fp16/CTestTestfile.ctest_examples_77_blackwell_fmha_gen_fp16.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp16")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp16"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_mla_2sm_fp16")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp16")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_fp16")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_mla_2sm_fp16" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_mla_2sm_fp16.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_mla_2sm_fp16/CTestTestfile.ctest_examples_77_blackwell_mla_2sm_fp16.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp16")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp16"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_mla_2sm_cpasync_fp16")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp16")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_2sm_cpasync_fp16")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_mla_2sm_cpasync_fp16" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_mla_2sm_cpasync_fp16.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_mla_2sm_cpasync_fp16/CTestTestfile.ctest_examples_77_blackwell_mla_2sm_cpasync_fp16.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp16")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp16"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_fmha_bwd_fp16")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp16")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_fmha_bwd_fp16")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_fmha_bwd_fp16" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_fmha_bwd_fp16.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_fmha_bwd_fp16/CTestTestfile.ctest_examples_77_blackwell_fmha_bwd_fp16.install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp16")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp16"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/77_blackwell_mla_fwd_fp16")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp16" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp16")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/77_blackwell_mla_fwd_fp16")
    endif()
  endif()
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/test/cutlass/ctest/ctest_examples_77_blackwell_mla_fwd_fp16" TYPE FILE RENAME "CTestTestfile.ctest_examples_77_blackwell_mla_fwd_fp16.cmake" FILES "/home/ubuntu/workspace/oyhj/FA/build/cutlass_build/examples/77_blackwell_fmha/ctest/ctest_examples_77_blackwell_mla_fwd_fp16/CTestTestfile.ctest_examples_77_blackwell_mla_fwd_fp16.install.cmake")
endif()

