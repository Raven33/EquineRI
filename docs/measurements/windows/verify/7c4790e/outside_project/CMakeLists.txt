cmake_minimum_required(VERSION 3.25)
project(OutsideApp LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

set(EQUINE_D3D12_BACKEND ON)
add_subdirectory("C:/Users/Arthu/Documents/GitHub/EquineNext" EquineRI)

add_executable(outside_app main.cpp)
target_link_libraries(outside_app PRIVATE EquineD3D12Backend EquineHelpersD3D12)
equine_add_shaders(outside_app PROFILE cs_6_6 SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fill.hlsl")
equine_deploy_agility_sdk(outside_app)
