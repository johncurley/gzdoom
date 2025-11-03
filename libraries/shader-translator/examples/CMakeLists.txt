cmake_minimum_required(VERSION 3.10)

# Simple test example
add_executable(test_spirv_to_msl test_spirv_to_msl.cpp)
target_link_libraries(test_spirv_to_msl PRIVATE shader-translator)

message(STATUS "shader-translator examples configured")
