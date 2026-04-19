cmake_minimum_required(VERSION 3.16)

project(BaseBrawl)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------- SERVER ----------------
add_executable(server server.cpp)

target_link_libraries(server
    pthread
)

# ---------------- CLIENT ----------------
find_package(SFML 2.5 COMPONENTS graphics window system REQUIRED)

add_executable(client client.cpp)

target_link_libraries(client
    sfml-graphics
    sfml-window
    sfml-system
)
