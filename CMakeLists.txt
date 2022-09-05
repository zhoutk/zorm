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

include_directories(./thirds/sqlit3)
include_directories(./zorm/src/include)
include_directories(./thirds/zjson)
include_directories(./thirds/MySQL-C-Api-6.1/include)

link_directories(./thirds/MySQL-C-Api-6.1/lib)

FILE(GLOB_RECURSE SRC_FILES "./src/*.cc" "./thirds/sqlit3/*.c") 
FILE(GLOB_RECURSE HEAD_FILES "./src/*.h" "./thirds/sqlit3/*.h") 

add_executable(${PROJECT_NAME} ${SRC_FILES} ${HEAD_FILES} )
target_link_libraries(${PROJECT_NAME} zorm dl)

add_subdirectory(zorm)
set_target_properties(zorm PROPERTIES LINKER_LANGUAGE CXX)