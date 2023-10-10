windres msys2_fish_start.rc -O coff -o msys2_fish_start.res
gcc -std=c11 -Wall -Wextra -Werror -static -municode -mwindows -o msys2_fish_start.exe msys2_fish_start.c msys2_fish_start.res
