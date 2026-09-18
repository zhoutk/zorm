cmake_minimum_required(VERSION 3.18)
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
	add_compile_options(/wd26451 /wd26812 /wd4267)
    #set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    #set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
elseif(UNIX AND NOT APPLE)
    add_definitions(-D__LINUX__)
    find_package(Threads REQUIRED)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
endif()

add_definitions(-DZORM_LIB)
add_definitions(-DSQLITE_OMIT_TEMPDB)
add_definitions(-DDM64)

include_directories(./thirds/sqlit3)
link_directories(./thirds/sqlit3/lib)
include_directories(./src/include)
include_directories(./thirds/zjson)
if(MSVC)
    include_directories(./thirds/MySQL-C-Api-6.1/include)
    link_directories(./thirds/MySQL-C-Api-6.1/lib)
    include_directories(./thirds/pgsql/include)
    link_directories(./thirds/pgsql/lib)
    include_directories(./thirds/dm8/dpi/include)
    link_directories(./thirds/dm8/dpi)
    set(MysqlDll libmysql.dll)
    set(pqName libpq.dll)
    set(sqliteName sqlite3.dll)
    set(dm8Name dmdpi.dll)
elseif(MINGW)
    # MySQL client: use MariaDB Connector/C from the clang64 toolchain
    # (package mingw-w64-clang-x86_64-libmariadbclient). It is a drop-in
    # replacement for the MySQL C API whose TLS layer is backed by OpenSSL 3
    # (TLSv1.2/1.3), unlike the bundled yaSSL-era Connector C 6.1 which
    # cannot handshake with MySQL 8 servers that require modern TLS.
    #
    # The paths are resolved to real Windows locations at configure time:
    # MSYS-style paths (/clang64/...) only work when the build is launched
    # from an MSYS shell, which converts them when spawning native tools -
    # a plain `cmake --build` (e.g. from VSCode) would not.
    find_path(ZORM_MARIADB_INCLUDE_DIR mysql.h
        HINTS $ENV{MINGW_PREFIX}/include/mysql
        PATHS /clang64/include/mysql
              D:/msys64/clang64/include/mysql
              C:/msys64/clang64/include/mysql
              D:/msys64/usr/../clang64/include/mysql
        NO_DEFAULT_PATH
        DOC "mysql.h of MariaDB Connector/C (pacman -S mingw-w64-clang-x86_64-libmariadbclient)")
    find_library(ZORM_MARIADB_LIBRARY NAMES mariadb libmariadb
        HINTS $ENV{MINGW_PREFIX}/lib
        PATHS /clang64/lib
              D:/msys64/clang64/lib
              C:/msys64/clang64/lib
        NO_DEFAULT_PATH)
    if(NOT ZORM_MARIADB_INCLUDE_DIR OR NOT ZORM_MARIADB_LIBRARY)
        message(FATAL_ERROR "MariaDB Connector/C not found - install it with: pacman -S mingw-w64-clang-x86_64-libmariadbclient, then reconfigure")
    endif()
    message(STATUS "MariaDB client: ${ZORM_MARIADB_LIBRARY}")
    include_directories(${ZORM_MARIADB_INCLUDE_DIR})
    include_directories(./thirds/pgsql/include)
    link_directories(./thirds/pgsql/lib)
    include_directories(./thirds/dm8/dpi/include)
    link_directories(./thirds/dm8/dpi)
    set(MysqlDll ${ZORM_MARIADB_LIBRARY})
    set(pqName -l:libpq.lib)
    set(sqliteName -l:sqlite3.lib)
    set(dm8Name -l:dmdpi.dll)
elseif(APPLE)
    link_directories(./thirds/MySQL-C-Api-6.1/lib)
    include_directories(./thirds/pgsql/macos/include)
    link_directories(./thirds/pgsql/macos/lib)
    set(MysqlDll libmysqlclient.a)
    set(pqName libpq.a)
elseif(UNIX)
    include_directories(/usr/include/postgresql)
    include_directories(/opt/dm8/dpi/include)
    link_directories(/opt/dm8/dpi)
    set(MysqlDll mysqlclient)
    set(pthread pthread)
    set(pqName pq)
    set(sqliteName libsqlite3.so)
    set(dm8Name libdmdpi.so)
endif()

FILE(GLOB_RECURSE SRC_FILES "./src/*.cc") 
FILE(GLOB_RECURSE HEAD_FILES "./src/*.h") 

add_executable(${PROJECT_NAME} ${SRC_FILES} ${HEAD_FILES})
if(MSVC)
    target_link_libraries(${PROJECT_NAME} ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name})
elseif(MINGW)
    target_link_libraries(${PROJECT_NAME} ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name})
elseif(UNIX)
    target_link_libraries(${PROJECT_NAME} ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} dl)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Output all generated binaries to ${CMAKE_SOURCE_DIR}/bin
set_target_properties(${PROJECT_NAME} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
)

##--------------------- Build unit tests -----------------------------------##
option(CMAKE_ZJSON_ENABLE_TEST "Whether to enable unit tests" ON)
if (CMAKE_ZJSON_ENABLE_TEST)
    message(STATUS "Unit tests enabled")
    enable_testing()

    add_subdirectory(./thirds/googletest-1.18.0 googletest-build EXCLUDE_FROM_ALL)
    include_directories(./thirds/googletest-1.18.0/googletest/include)
    include_directories(./thirds/googletest-1.18.0/googlemock/include)

    add_executable(test_sqlite3 tests/test_sqlite3.cpp src/JsonFileDb.cc)
    add_executable(test_mysql tests/test_mysql.cpp src/JsonFileDb.cc)
    add_executable(test_postgres tests/test_postgres.cpp src/JsonFileDb.cc)
    add_executable(test_dm8 tests/test_dm8.cpp src/JsonFileDb.cc)
    add_executable(test_jsonfile tests/test_jsonfile.cpp src/JsonFileDb.cc)

    # Output test executables to ${CMAKE_SOURCE_DIR}/bin as well
    set_target_properties(test_sqlite3 test_mysql test_postgres test_dm8 test_jsonfile PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    )

    add_test(NAME test_sqlite3 COMMAND test_sqlite3)
    add_test(NAME test_mysql COMMAND test_mysql)
    add_test(NAME test_postgres COMMAND test_postgres)
    add_test(NAME test_dm8 COMMAND test_dm8)
    add_test(NAME test_jsonfile COMMAND test_jsonfile)
    if(MSVC)
        target_link_libraries(test_sqlite3 ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_mysql ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_postgres ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_dm8 ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_jsonfile ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
    elseif(MINGW)
        target_link_libraries(test_sqlite3 ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_mysql ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_postgres ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_dm8 ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
        target_link_libraries(test_jsonfile ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main)
    elseif(UNIX)
        target_link_libraries(test_sqlite3 ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main dl)
        target_link_libraries(test_mysql ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main dl)
        target_link_libraries(test_postgres ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main dl)
        target_link_libraries(test_dm8 ${pthread} ${MysqlDll} ${pqName} ${sqliteName} ${dm8Name} gtest gtest_main dl)
        target_link_libraries(test_jsonfile gtest gtest_main pthread dl)
    endif()
endif()