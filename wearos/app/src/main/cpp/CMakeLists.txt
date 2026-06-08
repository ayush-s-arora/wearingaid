cmake_minimum_required(VERSION 3.22.1)

project("wearingaid_jni")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../audio-engine audio_engine_build)

add_library(${CMAKE_PROJECT_NAME} SHARED
        NativeAudioEngine_jni.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        audio_engine
        android
        log)