windres msys2_fish_launcher.rc -O coff -o msys2_fish_launcher.res
gcc -std=c11 -Wall -Wextra -static -municode -mwindows -o ucrt64_fish_launcher.exe msys2_fish_launcher.c msys2_fish_launcher.res
::removed -Werror 

if %errorlevel%==0 (
   echo "Sucess!"
   cp ucrt64_fish_launcher.exe c:/msys64
   start c:/msys64/ucrt64_fish_launcher.exe
)
