# MyFindWin
find/list all or specific files, grep and grep -I (case insensitive)

usage: .\MyFindWin.exe <number of threads> <path> <search_string> [grep|grepI|find]
default is find; use * as <search_string> to list all files; Output is in output.txt

e.g. List all files in C:\:
  - .\MyFindWin.exe 64 C:\ *

only files with xyz in filename:
  - .\MyFindWin.exe 64 c:\ xyz
  
grep for xyz in files:
  - .\MyFindWin.exe 64 C:\ xyz grep
  - case insensitive:  .\MyFindWin.exe 64 C:\ xyz grep
  
  
