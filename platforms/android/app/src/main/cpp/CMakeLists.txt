cmake_minimum_required(VERSION 3.22)
project(partyboard_quest CXX)

# Meta Quest: the headset side of the game (quest_xr.cpp). The game itself is
# libmain.so, built by the repository's CMake presets and staged into jniLibs;
# this library only shows its window in the headset and reads the controllers.
find_package(OpenXR REQUIRED CONFIG)

add_library(partyboard_quest SHARED quest_xr.cpp stereo_view.cpp table_anchor.cpp)
target_compile_features(partyboard_quest PRIVATE cxx_std_20)
target_link_libraries(partyboard_quest PRIVATE OpenXR::openxr_loader android nativewindow log EGL GLESv3)
