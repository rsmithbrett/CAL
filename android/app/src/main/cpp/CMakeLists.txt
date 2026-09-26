cmake_minimum_required(VERSION 3.22.1)
project(discovercards LANGUAGES CXX)

add_library(discovercards SHARED native_cards.cpp)
target_include_directories(discovercards PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/android_compat
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../App)
target_compile_features(discovercards PRIVATE cxx_std_17)
