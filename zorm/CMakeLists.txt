cmake_minimum_required(VERSION 3.8)
file(READ "version" ver)
string(REGEX MATCH "VERSION_MAJOR ([0-9]*)" _ ${ver})
set(ver_major ${CMAKE_MATCH_1})
string(REGEX MATCH "VERSION_MINOR ([0-9]*)" _ ${ver})
set(ver_minor ${CMAKE_MATCH_1})
string(REGEX MATCH "VERSION_PATCH ([0-9]*)" _ ${ver})
set(ver_patch ${CMAKE_MATCH_1})
message("version: ${ver_major}.${ver_minor}.${ver_patch}")
project(zorm VERSION ${ver_major}.${ver_minor}.${ver_patch})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# set(CMAKE_BUILD_TYPE Debug)  # Use `cmake -DCMAKE_BUILD_TYPE=Debug` more better
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

if(MSVC)
    add_definitions(-D__MSVC__)
	string(APPEND CMAKE_CXX_FLAGS " /Zc:__cplusplus")
	add_compile_options(/wd26451 /wd26812)

    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")

    set(MysqlDll libmysql.dll)
elseif(APPLE)
    set(MysqlDll libmysqlclient.a)
elseif(UNIX)
    add_definitions(-D__LINUX__)
    find_package(Threads REQUIRED)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

    set(MysqlDll mysqlclient)
    set(pthread pthread)
    set(pqxxName pqxx)
    set(pqName pq)
endif()

include_directories(../thirds/sqlit3)
include_directories(../thirds/MySQL-C-Api-6.1/include)
include_directories(../thirds/zjson)

if(APPLE OR NOT UNIX)
    link_directories(../thirds/MySQL-C-Api-6.1/lib)
endif()

FILE(GLOB_RECURSE SRC_FILES "./*.cc" "../thirds/sqlit3/*.c") 
FILE(GLOB_RECURSE HEAD_FILES "./*.h" "../thirds/sqlit3/*.h") 

ADD_LIBRARY(${PROJECT_NAME} SHARED ${SRC_FILES} ${HEAD_FILES})
target_link_libraries(${PROJECT_NAME} ${pthread} ${MysqlDll} ${pqxxName} ${pqName})