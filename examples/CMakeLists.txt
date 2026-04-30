cmake_minimum_required(VERSION 3.14)

add_executable(performer performer.cpp)
target_link_libraries(performer PRIVATE zmqae)

add_executable(handler handler.cpp)
target_link_libraries(handler PRIVATE zmqae)
