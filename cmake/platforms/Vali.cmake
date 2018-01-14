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

# Setup environment stuff for cmake configuration
set(CMAKE_CROSSCOMPILING True)
set(CMAKE_C_COMPILER "$ENV{CROSS}/bin/clang" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "$ENV{CROSS}/bin/clang++" CACHE FILEPATH "")
set(CMAKE_LINKER "$ENV{CROSS}/bin/lld-link" CACHE FILEPATH "")
set(MOLLENOS True)
set(VERBOSE 1)

# Setup cmake rules, we do not care for default stuff
# CMAKE_C_CREATE_SHARED_LIBRARY && CMAKE_CXX_CREATE_SHARED_LIBRARY
# CMAKE_C_ARCHIVE_CREATE && CMAKE_CXX_ARCHIVE_CREATE (CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS)
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> $ENV{LIBRARIES}/libcrt.lib <OBJECTS> /out:<TARGET> /entry:__CrtConsoleEntry <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_LINKER> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> $ENV{LIBRARIES}/libcxx.lib <OBJECTS> /out:<TARGET> /entry:__CrtConsoleEntry <LINK_LIBRARIES>")

# Must have a local host llvm-tblgen in path
set(LLVM_TABLEGEN "$ENV{CROSS}/bin/llvm-tblgen" CACHE FILEPATH "")
set(LLVM_DEFAULT_TARGET_TRIPLE i386-pc-win32-itanium-coff)
set(LLVM_ENABLE_EH True)
set(LLVM_ENABLE_RTTI True)
set(LLVM_USE_LINKER lld)

# Disable tests and examples to speedup build process
set(LLVM_INCLUDE_TESTS Off)
set(LLVM_INCLUDE_EXAMPLES Off)

# Setup shared compile flags to make compilation succeed
set(COMPILE_FLAGS
    -U_WIN32
    -DMOLLENOS
    -Di386
    -m32
    -fms-extensions
    -Wall
    -nostdlib
    -nostdinc
    -O3
    -I$ENV{INCLUDES}/cxx
    -I$ENV{INCLUDES})

string(REPLACE ";" " " COMPILE_FLAGS "${COMPILE_FLAGS}")

# We need to preserve any flags that were passed in by the user. However, we
# can't append to CMAKE_C_FLAGS and friends directly, because toolchain files
# will be re-invoked on each reconfigure and therefore need to be idempotent.
# The assignments to the _INITIAL cache variables don't use FORCE, so they'll
# only be populated on the initial configure, and their values won't change
# afterward.
set(_CMAKE_C_FLAGS_INITIAL "${CMAKE_C_FLAGS}" CACHE STRING "")
set(CMAKE_C_FLAGS "${_CMAKE_C_FLAGS_INITIAL} ${COMPILE_FLAGS}" CACHE STRING "" FORCE)

set(_CMAKE_CXX_FLAGS_INITIAL "${CMAKE_CXX_FLAGS}" CACHE STRING "")
set(CMAKE_CXX_FLAGS "${_CMAKE_CXX_FLAGS_INITIAL} ${COMPILE_FLAGS}" CACHE STRING "" FORCE)

set(LINK_FLAGS
    /nodefaultlib
    /machine:X86 
    /subsystem:native,
    $ENV{LIBRARIES}/libclang.lib
    $ENV{LIBRARIES}/libm.lib
    $ENV{LIBRARIES}/libc.lib
    $ENV{LIBRARIES}/libunwind.lib)

string(REPLACE ";" " " LINK_FLAGS "${LINK_FLAGS}")

# See explanation for compiler flags above for the _INITIAL variables.
set(_CMAKE_EXE_LINKER_FLAGS_INITIAL "${CMAKE_EXE_LINKER_FLAGS}" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "${_CMAKE_EXE_LINKER_FLAGS_INITIAL} ${LINK_FLAGS}" CACHE STRING "" FORCE)

set(_CMAKE_MODULE_LINKER_FLAGS_INITIAL "${CMAKE_MODULE_LINKER_FLAGS}" CACHE STRING "")
set(CMAKE_MODULE_LINKER_FLAGS "${_CMAKE_MODULE_LINKER_FLAGS_INITIAL} ${LINK_FLAGS}" CACHE STRING "" FORCE)

set(_CMAKE_SHARED_LINKER_FLAGS_INITIAL "${CMAKE_SHARED_LINKER_FLAGS}" CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS "${_CMAKE_SHARED_LINKER_FLAGS_INITIAL} ${LINK_FLAGS}" CACHE STRING "" FORCE)

# CMake populates these with a bunch of unnecessary libraries, which requires
# extra case-correcting symlinks and what not. Instead, let projects explicitly
# control which libraries they require.
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
