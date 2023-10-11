windres msys2-fish-launcher.rc -O coff -o msys2-fish-launcher.res
gcc -std=c11 -Wall -Wextra -static -municode -mwindows -o ucrt64-fish-launcher.exe msys2-fish-launcher.c msys2-fish-launcher.res
::removed -Werror 

if %errorlevel%==0 (
   echo "Sucess!"
   cp ucrt64-fish-launcher.exe c:/msys64
   start c:/msys64/ucrt64-fish-launcher.exe
)
