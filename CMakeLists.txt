# CMAkeLists.txt file for mumpi project
cmake_minimum_required(VERSION 2.8.9)

# Options. Turn on with 'cmake -Dvarname=ON'.
option(test "Build all tests." OFF) # makes boolean 'test' available

project(mumpi)
add_subdirectory("${PROJECT_SOURCE_DIR}/deps/mumlib")

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

set (CMAKE_CXX_FLAGS "-std=c++11 -O3 ${CMAKE_CXX_FLAGS}")

if (MSYS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DLOG4CPP_FIX_ERROR_COLLISION=1 -D__USE_W32_SOCKETS")
endif ()

INCLUDE(FindPkgConfig)
find_package(PkgConfig REQUIRED)

# INCLUDES
include_directories(include)
include_directories("${PROJECT_SOURCE_DIR}/deps/mumlib/include")
if (MSYS)
include_directories(~/boost_1_72_0)
endif ()

# SOURCES
#Can manually add the sources using the set command as follows:
#set(SOURCES src/mainapp.cpp src/Student.cpp)
file(GLOB SOURCES "src/*.cpp")

add_executable(mumpi ${SOURCES})

# LINKING
target_link_libraries(mumpi portaudio)
target_link_libraries(mumpi mumlib)

# TESTING

#enable_testing()
#add_subdirectory("${PROJECT_SOURCE_DIR}/deps/googletest")

#file(GLOB TESTS "test/*.cpp")

#add_executable(runUnitTests ${TESTS})
#target_link_libraries(runUnitTests gtest gtest_main)
#add_test(NAME mumpi-test COMMAND runUnitTests)
