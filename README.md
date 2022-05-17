# MyFindWin

Can List non default NTFS Alternate Data Streams for Directories and Files

find/list all or specific files, grep and grep -I (case insensitive)

usage: .\MyFindWin.exe <number of threads> <path> <search_string> [grep|grepI|find]
default is find; use * as <search_string> to list all files; Output is in output.txt

e.g. List non-default (other than ::$DATA) NTFS Streams for dirs and files:
   - .\MyFindWin.exe 8 C:\ *
   
Output: 
```
NON default streamname: :xdg.origin.url:$DATA in FILE: C:\Users\All Users\Samsung\Backup\Samsung_Magician_ML_Setup_Backup.exe size: 100 
NON default streamname: :Win32App_1:$DATA in FOLDER: C:\Program Files\7-Zip size: 0 
NON default streamname: :xdg.referrer.url:$DATA in FILE: C:\Users\All Users\Samsung\Backup\Samsung_Magician_ML_Setup_Backup.exe size: 69  
NON default streamname: :xdg.referrer.url:$DATA in FILE: C:\Users\All Users\Samsung\Backup\Samsung_Magician_ML_Setup_Backup.exe size: 69 
NON default streamname: :Win32App_1:$DATA in FOLDER: C:\Program Files\Adblock Plus for IE size: 0   
NON default streamname: :Win32App_1:$DATA in FOLDER: C:\Program Files\FileZilla FTP Client size: 0
```  
  
e.g. List all files in C:\:
  - .\MyFindWin.exe 64 C:\ *

only files with xyz in filename:
  - .\MyFindWin.exe 64 c:\ xyz
  
grep for xyz in files:
  - .\MyFindWin.exe 64 C:\ xyz grep
  - case insensitive:  .\MyFindWin.exe 64 C:\ xyz grep
  
  
