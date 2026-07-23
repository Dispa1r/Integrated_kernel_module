cmake_minimum_required(VERSION 3.10)
project(rfrida-zygisk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fvisibility=hidden -fomit-frame-pointer -fno-rtti -fno-exceptions -Wall")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections")

add_library(rfrida_zygisk SHARED
    main.cpp
    custom_loader.cpp
)

target_link_libraries(rfrida_zygisk android log dl)
