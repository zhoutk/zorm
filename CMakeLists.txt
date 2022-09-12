cmake_minimum_required(VERSION 3.8)

project(main)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_BUILD_TYPE Debug)  # Use `cmake -DCMAKE_BUILD_TYPE=Debug` more better
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

if(MSVC)
    add_definitions(-D__MSVC__)
	string(APPEND CMAKE_CXX_FLAGS " /Zc:__cplusplus")
	add_compile_options(/wd26451 /wd26812)

    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
elseif(UNIX)
    add_definitions(-D__LINUX__)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
endif()

add_definitions(-DZORM_LIB)

include_directories(./thirds/sqlit3)
include_directories(./zorm/src/include)
include_directories(./thirds/zjson)
include_directories(./thirds/MySQL-C-Api-6.1/include)

link_directories(./thirds/MySQL-C-Api-6.1/lib)

FILE(GLOB_RECURSE SRC_FILES "./src/*.cc" "./thirds/sqlit3/*.c") 
FILE(GLOB_RECURSE HEAD_FILES "./src/*.h" "./thirds/sqlit3/*.h") 

add_executable(${PROJECT_NAME} ${SRC_FILES} ${HEAD_FILES} )
if(MSVC)
    target_link_libraries(${PROJECT_NAME} zorm)
elseif(UNIX)
    target_link_libraries(${PROJECT_NAME} zorm dl)
endif()

add_subdirectory(zorm)
set_target_properties(zorm PROPERTIES LINKER_LANGUAGE CXX)

##--------------------- Build unit tests -----------------------------------##
option(CMAKE_ZJSON_ENABLE_TEST "Whether to enable unit tests" ON)
if (CMAKE_ZJSON_ENABLE_TEST)
    message(STATUS "Unit tests enabled")
    enable_testing()
    add_subdirectory(thirds/googletest-release-1.11.0 EXCLUDE_FROM_ALL)
    include_directories(thirds/googletest-release-1.11.0/googletest/include)

    add_executable(test_sqlite3 tests/test_sqlite3.cpp "./thirds/sqlit3/sqlite3.h" "./thirds/sqlit3/sqlite3.c")
    if(MSVC)
        target_link_libraries(test_sqlite3 gtest gtest_main)
        add_test(NAME test_sqlite3 COMMAND test_sqlite3)
    elseif(UNIX)
        target_link_libraries(test_sqlite3 gtest gtest_main dl)
        add_test(NAME test_sqlite3 COMMAND test_sqlite3 dl)
    endif()
endif()