cmake_minimum_required(VERSION 3.22)
project(WearingAidAudioEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

target_include_directories(audio_engine PUBLIC include third_party)

add_library(audio_engine STATIC
	src/audio_engine.cpp
)

# Test locally
add_executable(test_engine src/main.cpp)
target_link_libraries(test_engine PRIVATE audio_engine)
