windres msys2_fish_start.rc -O coff -o msys2_fish_start.res
g++ -std=c++11 -Wall -Wextra -static -municode -mwindows -o msys2_fish_start.exe msys2_fish_start.cpp msys2_fish_start.res

cp msys2_fish_start.exe c:/msys64
start c:/msys64/msys2_fish_start.exe

::removed -std=c11 -Werror 
