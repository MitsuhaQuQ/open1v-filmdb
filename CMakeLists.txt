cmake_minimum_required(VERSION 3.20)
project(open1v-filmdb VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
set(FILMDB_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)
add_subdirectory(external/open1V-cli EXCLUDE_FROM_ALL)
set(BUILD_TESTING ${FILMDB_BUILD_TESTING})

add_executable(film-record
    src/application.cpp
    src/database.cpp
    src/film_records.cpp
    src/main.cpp)
target_include_directories(film-record PRIVATE include)
target_link_libraries(film-record PRIVATE open1v-core)
if(WIN32)
    target_link_libraries(film-record PRIVATE winsqlite3)
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(film-record PRIVATE SQLite3::SQLite3)
endif()

if(BUILD_TESTING)
    add_test(NAME film-record-self-test COMMAND film-record self-test)
endif()
