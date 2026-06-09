cmake_minimum_required(VERSION 3.22)
project(wearingaidaudioengine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/pffft)

add_library(audio_engine STATIC
	src/audio_engine.cpp
)

target_include_directories(audio_engine PUBLIC include third_party third_party/pffft/include/pffft)
target_link_libraries(audio_engine PUBLIC PFFFT::PFFFT)

# Engine diagnostics call __android_log_print when built for Android (see ELOG in
# audio_engine.cpp). That symbol lives in the NDK's liblog; link it on Android only.
if(ANDROID)
	target_link_libraries(audio_engine PUBLIC log)
endif()

# Test locally
add_executable(test_engine src/main.cpp)
target_link_libraries(test_engine PRIVATE audio_engine)