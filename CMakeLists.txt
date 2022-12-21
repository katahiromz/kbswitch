# CMakeLists.txt --- CMake project settings
#    ex) cmake -G "Visual Studio 9 2008" .
#    ex) cmake -DCMAKE_BUILD_TYPE=Release -G "MSYS Makefiles" .
##############################################################################

# CMake minimum version
cmake_minimum_required(VERSION 3.0)

# project name and languages
project(MyProject C RC)

##############################################################################

# kbsdll.dll
add_library(kbsdll SHARED kbsdll.c)
set_target_properties(kbsdll PROPERTIES PREFIX "")

# kbswitch.exe
add_executable(kbswitch kbswitch.c kbswitch_res.rc kbsdll.def)
target_link_libraries(kbswitch comctl32 shell32 imm32)

##############################################################################
