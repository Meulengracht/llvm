# Cross toolchain configuration for using clang-cl on non-Windows hosts to
# target MSVC.
#
# Usage:
# cmake -G Ninja
#    -DCMAKE_TOOLCHAIN_FILE=/path/to/this/file
#    -DHOST_ARCH=[aarch64|arm64|armv7|arm|i686|x86|x86_64|x64]
#    -DMSVC_BASE=/path/to/MSVC/system/libraries/and/includes
#    -DWINSDK_BASE=/path/to/windows-sdk
#    -DWINSDK_VER=windows sdk version folder name
#
# HOST_ARCH:
#    The architecture to build for.
#
# CROSS:
#   *Absolute path* to a folder containing the toolchain which will be used to
#   build.  At a minimum, this folder should have a bin directory with a
#   copy of clang, clang++, and lld-link, as well as a lib directory
#   containing clang's system resource directory.
#

# Make sure all the proper env are set
if(NOT DEFINED ENV{CROSS})
    message(FATAL_ERROR "CROSS environmental variable is not defined")
endif()
if(NOT DEFINED ENV{VALI_ARCH})
    message(FATAL_ERROR "VALI_ARCH environmental variable is not defined")
endif()
if(NOT DEFINED ENV{VALI_INCLUDES})
    message(FATAL_ERROR "VALI_INCLUDES environmental variable is not defined")
endif()
if(NOT DEFINED ENV{VALI_LIBRARIES})
    message(FATAL_ERROR "VALI_LIBRARIES environmental variable is not defined")
endif()

# Setup environment stuff for cmake configuration
set(CMAKE_SYSTEM_NAME Vali)
set(CMAKE_CROSSCOMPILING OFF CACHE BOOL "")
set(CMAKE_C_COMPILER "$ENV{CROSS}/bin/clang" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "$ENV{CROSS}/bin/clang++" CACHE FILEPATH "")
set(CMAKE_LINKER "$ENV{CROSS}/bin/lld-link" CACHE FILEPATH "")
set(ENV{INCLUDES} "$ENV{VALI_INCLUDES}")
set(ENV{LIBRARIES} "$ENV{VALI_LIBRARIES}")
set(VERBOSE 1)

##################################################
# Setup LLVM custom options
##################################################

# Must have a local host llvm-tblgen in path
set(LLVM_TABLEGEN "$ENV{CROSS}/bin/llvm-tblgen" CACHE FILEPATH "")

if("$ENV{VALI_ARCH}" STREQUAL "i386")
    set(LLVM_BUILD_32_BITS ON CACHE BOOL "")
    set(LLVM_DEFAULT_TARGET_TRIPLE i386-pc-win32-itanium-coff)
else()
    set(LLVM_BUILD_32_BITS OFF CACHE BOOL "")
    set(LLVM_DEFAULT_TARGET_TRIPLE amd64-pc-win32-itanium-coff)
endif()

set(LLVM_TARGET_ARCH "X86" CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "") 

set(LLVM_ENABLE_EH ON CACHE BOOL "")
set(LLVM_ENABLE_RTTI ON CACHE BOOL "")

# Disable tests and examples to speedup build process
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "")

# Setup shared compile flags to make compilation succeed
if("$ENV{VALI_ARCH}" STREQUAL "i386")
    set(COMPILE_FLAGS -U_WIN32 -DMOLLENOS -DZLIB_DLL -Di386 -D__i386__ -m32 -fms-extensions
        -Wall -nostdlib -nostdinc -O3 -Xclang -flto-visibility-public-std -I$ENV{VALI_INCLUDES}/cxx -I$ENV{VALI_INCLUDES})
else()
    set(COMPILE_FLAGS -U_WIN32 -DMOLLENOS -DZLIB_DLL -Damd64 -D__amd64__ -m64 -fms-extensions
        -Wall -nostdlib -nostdinc -O3 -Xclang -flto-visibility-public-std -fdwarf-exceptions -I$ENV{VALI_INCLUDES}/cxx -I$ENV{VALI_INCLUDES})
endif()

string(REPLACE ";" " " COMPILE_FLAGS "${COMPILE_FLAGS}")

# We need to preserve any flags that were passed in by the user. However, we
# can't append to CMAKE_C_FLAGS and friends directly, because toolchain files
# will be re-invoked on each reconfigure and therefore need to be idempotent.
# The assignments to the _INITIAL cache variables don't use FORCE, so they'll
# only be populated on the initial configure, and their values won't change
# afterward.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMPILE_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILE_FLAGS}" CACHE STRING "" FORCE)

if("$ENV{VALI_ARCH}" STREQUAL "i386")
    set(LINK_FLAGS /nodefaultlib /machine:X86 /subsystem:native /lldmap
        $ENV{VALI_LIBRARIES}/libclang.lib $ENV{VALI_LIBRARIES}/libm.lib $ENV{VALI_LIBRARIES}/libc.lib
        $ENV{VALI_LIBRARIES}/zlib.lib $ENV{VALI_LIBRARIES}/libunwind.lib)
else()
    set(LINK_FLAGS /nodefaultlib /machine:X64 /subsystem:native /lldmap
        $ENV{VALI_LIBRARIES}/libclang.lib $ENV{VALI_LIBRARIES}/libm.lib $ENV{VALI_LIBRARIES}/libc.lib
        $ENV{VALI_LIBRARIES}/zlib.lib $ENV{VALI_LIBRARIES}/libunwind.lib)
endif()

string(REPLACE ";" " " LINK_FLAGS "${LINK_FLAGS}")

# See explanation for compiler flags above for the _INITIAL variables.
set(_CMAKE_EXE_LINKER_FLAGS_INITIAL "${CMAKE_EXE_LINKER_FLAGS}" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "${LINK_FLAGS}" CACHE STRING "" FORCE)

set(_CMAKE_MODULE_LINKER_FLAGS_INITIAL "${CMAKE_MODULE_LINKER_FLAGS}" CACHE STRING "")
set(CMAKE_MODULE_LINKER_FLAGS "${_CMAKE_MODULE_LINKER_FLAGS_INITIAL} ${LINK_FLAGS}" CACHE STRING "" FORCE)

set(_CMAKE_SHARED_LINKER_FLAGS_INITIAL "${CMAKE_SHARED_LINKER_FLAGS}" CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS "${_CMAKE_SHARED_LINKER_FLAGS_INITIAL} ${LINK_FLAGS}" CACHE STRING "" FORCE)

# CMake populates these with a bunch of unnecessary libraries, which requires
# extra case-correcting symlinks and what not. Instead, let projects explicitly
# control which libraries they require.
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")