#------------------------------------------------------------------------------------------------------------
#
# Automate-VCPKG by Andre Taulien   modified by T K Sharpless
# ===============================
#
# Project Repository: https://github.com/REGoth-project/Automate-VCPKG
# License ..........: MIT, see end of file.
#
# Based on: https://github.com/sutambe/cpptruths/blob/vcpkg_cmake_blog/cpp0x/vcpkg_test/CMakeLists.txt
#
# 
# While [Vcpkg](https://github.com/microsoft/vcpkg) on it's own is awesome, it does add 
# a little bit of complexity to getting a project to build. Even more if the one trying 
# to compile your application is not too fond of the commandline. Additionally, CMake 
# commands tend to get rather long with the toolchain path. 
# 
# To keep things simple for new users who just want to get the project to build, this 
# script offers a solution.
# 
# Lets assume your main `CMakelists.txt` looks something like this:
#
#     cmake_minimum_required (VERSION 3.12.0)
#     project (MyProject)
#     
#     add_executable(MyExecutable main.c)
# 
# To integrate Vcpkg into that `CMakelists.txt`, simple put the following lines before the 
# call to `project(MyProject)`:
# 
#     include(cmake/automate-vcpkg.cmake)
#     
#     vcpkg_bootstrap()
#     vcpkg_install_packages(libsquish physfs)
# 
# The call to `vcpkg_bootstrap()` will clone the official Vcpkg repository and bootstrap it. 
# If it detected an existing environment variable defining a valid `VCPKG_ROOT`, it will 
# update the existing installation of Vcpkg.
# 
# Arguments to `vcpkg_install_packages()` are the packages you want to install using Vcpkg.
# 
# If you want to keep the possibility for users to chose their own copy of Vcpkg, you can 
# simply not run the code snippet mentioned above, something like this will work:
# 
#     option(SKIP_AUTOMATE_VCPKG "When ON, you will need to built the packages 
#      required by MyProject on your own or supply your own vcpkg toolchain.")
#     
#     if (NOT SKIP_AUTOMATE_VCPKG)
#       include(cmake/automate-vcpkg.cmake)
#     
#       vcpkg_bootstrap()
#       vcpkg_install_packages(libsquish physfs)
#     endif()
#  
# Then, the user has to supply the packages on their own, be it through Vcpkg or manually 
# specifying their locations.
#------------------------------------------------------------------------------------------------------------

cmake_minimum_required (VERSION 3.12)


macro(_install_or_update_vcpkg)
    if(NOT EXISTS ${VCPKG_ROOT})
        message(STATUS "Cloning vcpkg in ${VCPKG_ROOT}")
        execute_process(COMMAND git clone https://github.com/Microsoft/vcpkg.git ${VCPKG_ROOT})

        # If a reproducible build is desired (and potentially old libraries are # ok), uncomment the
        # following line and pin the vcpkg repository to a specific githash.
        # execute_process(COMMAND git checkout 745a0aea597771a580d0b0f4886ea1e3a94dbca6 WORKING_DIRECTORY ${VCPKG_ROOT})
    else()
        # The following command has no effect if the vcpkg repository is in a detached head state.
        message(STATUS "Auto-updating vcpkg in ${VCPKG_ROOT}")
        execute_process(COMMAND git pull WORKING_DIRECTORY ${VCPKG_ROOT})
    endif()

    if(NOT EXISTS ${VCPKG_ROOT}/README.md)
        message(FATAL_ERROR "***** FATAL ERROR: Could not clone vcpkg *****")
    endif()

    if(WIN32)
        set(VCPKG_EXEC ${VCPKG_ROOT}/vcpkg.exe)
        set(VCPKG_BOOTSTRAP ${VCPKG_ROOT}/bootstrap-vcpkg.bat)
    else()
        set(VCPKG_EXEC ${VCPKG_ROOT}/vcpkg)
        set(VCPKG_BOOTSTRAP ${VCPKG_ROOT}/bootstrap-vcpkg.sh)
    endif()

    if(NOT EXISTS ${VCPKG_EXEC})
        message("Bootstrapping vcpkg in ${VCPKG_ROOT}")
        execute_process(COMMAND ${VCPKG_BOOTSTRAP} WORKING_DIRECTORY ${VCPKG_ROOT})
    endif()

    if(NOT EXISTS ${VCPKG_EXEC})
        message(FATAL_ERROR "***** FATAL ERROR: Could not bootstrap vcpkg *****")
    endif()
   
endmacro()

# Installs the list of packages given as parameters using Vcpkg
macro(vcpkg_install_packages)

    message("Installing/Updating vcpkg packages: ")

    if (VCPKG_TARGET_TRIPLET)
        set(ENV{VCPKG_DEFAULT_TRIPLET} "${VCPKG_TARGET_TRIPLET}")
    endif()

    foreach(PKG ${ARGN})
        message(STATUS "---- ${PKG} ---- ")
        execute_process(
            COMMAND ${VCPKG_EXEC} install ${PKG}
            WORKING_DIRECTORY ${VCPKG_ROOT}
            OUTPUT_QUIET
            RESULT_VARIABLE RES
            )
        if( ${RES} )  ## assume zero/null means OK
            if(AUTOMATE_VCPKG_UPDATE)  # try updating
                execute_process(
                    COMMAND ${VCPKG_EXEC} install ${PKG} --recurse
                    WORKING_DIRECTORY ${VCPKG_ROOT}
                    RESULT_VARIABLE RES
                )
                if( ${RES} )
                    message(FATAL_ERROR "update ${PKG} failed")
                endif()
            else()
                message(WARNING "install ${PKG} failed -- may want update")
            endif()
        endif()
    endforeach()

endmacro()
    
# if user did not supply their own VCPKG toolchain file
# set default directory and toolchain names
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VCPKG_ROOT})
        if(WIN32)
            set(VCPKG_ROOT ${CMAKE_CURRENT_BINARY_DIR}/vcpkg)
        else()
            set(VCPKG_ROOT ${CMAKE_CURRENT_BINARY_DIR}/.vcpkg)
        endif()
    else()
        set(VCPKG_ROOT $ENV{VCPKG_ROOT})
    endif()
    # set the toolchain file name
    set(CMAKE_TOOLCHAIN_FILE ${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake CACHE STRING "")
    message("New CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE}")
else()
    message("Existing CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE}")
    string(REPLACE "/scripts/buildsystems/vcpkg.cmake" "" TPTH ${CMAKE_TOOLCHAIN_FILE}) 
    if(NOT DEFINED VCPKG_ROOT)
        set(VCPKG_ROOT ${TPTH})
    elseif( NOT "${VCPKG_ROOT}" EQUAL "${TPTH}")
        message(FATAL_ERROR "but existing VCPKG_ROOT is ${VCPKG_ROOT}")
    endif()
endif()

# make sure we have a working vcpkg instance
_install_or_update_vcpkg()
message(STATUS "AUTOMATE_VCPKG variables --")
message(STATUS "  VCPKG_ROOT:         ${VCPKG_ROOT}")
message(STATUS "  VCPKG_EXEC:         ${VCPKG_EXEC}")
message(STATUS "  VCPKG_BOOTSTRAP:    ${VCPKG_BOOTSTRAP}")
if (NOT ${VCPKG_TARGET_TRIPLET} EQUAL x64-windows)
    message("VCPKG_TARGET_TRIPLET is ${VCPKG_TARGET_TRIPLET}, changing to x64-windows")
    set(VCPKG_TARGET_TRIPLET x64-windows)
endif()
  
##message(FATAL_ERROR "EXIT DEBUG TEST")

# MIT License
# 
# Copyright (c) 2019 REGoth-project
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.