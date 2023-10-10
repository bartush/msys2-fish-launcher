windres msys2_fish_start.rc -O coff -o msys2_fish_start.res
gcc -o msys2_fish_start.exe msys2_fish_start.c msys2_fish_start.res
