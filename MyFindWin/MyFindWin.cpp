/*
 ============== my find grep: concurrent find respectively concurrent grep ==============

usage:
.\MyFindWin.exe <number of threads> <path> <search_string> [find|grepCPP|grepCPPI]  # defaults to find

    if you use * as <search_string>: it lists ALL STREAMS for FILES and FOLDERS except ::$DATA and all files in output.txt

    else: Program will save found files in: "output.txt"

- find: C++ string search on whole file path (i.e. find xyz will match both: c:\Users\xyz\Desktop\xyz.txt)
- grepCPP: using C++ searches binary files
- grepCPPI: using C++ skipping binary files (like grep -I)

also searching in file path part not just after last  more like find -type f <path> | grep <search_string>


TODO:

- fix problem with LONG PATHS: \\?\D:\DROPPYBACKUP20220402\20220402_204329\Dropbox\saved Websites\Derivatives Analytics with Python  Data Analysis, Models, Simulation, Calibration and Hedging (The Wiley Finance Series) eBook  Yves Hilpisch  Amazon.de  Kindle-Shop-Dateien

*/
#include <Windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <atlstr.h>
#include <WinBase.h>
#include <fileapi.h>

#include <memory.h>

#include <sys/stat.h>
#include <fcntl.h>
//#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <locale>
#include <chrono>
#include <filesystem>
#include <functional>  // std::ref
#include <iostream>
#include <fstream>
#include <thread>
#include <array>
#include <vector>

#include <mutex>
#include <exception>
#include <cstdlib>
//#include <dirent.h> 
#include <sys/stat.h>
#include <chrono>
#include <typeinfo>
#include <atomic>
#include <ctime>
#include <new>


#include <string>


thread_local std::string tls_path;
thread_local std::vector<std::string> tls_filenames;
thread_local std::vector<std::string> tls_directories;

std::string arg_source, arg_dest;

// global filename and directory vectors & mutexes
// each thread puts directories into global vectors

#define NR_GLOBAL_FILENAME_MUTEXES 8 // also works surprisingly well with 1
std::array<std::mutex, NR_GLOBAL_FILENAME_MUTEXES> global_filename_mutexes; // mutexes for the following:
std::array<std::vector<std::string>, NR_GLOBAL_FILENAME_MUTEXES> filename_array_of_vectors;

#define NR_GLOBAL_DIRECTORIES_MUTEXES 8 // also works surprisingly well with 1
std::array<std::mutex, NR_GLOBAL_DIRECTORIES_MUTEXES> global_directories_mutexes; // mutexes for the following:
std::array<std::vector<std::string>, NR_GLOBAL_DIRECTORIES_MUTEXES> directories_array_of_vectors;

// global vectors for initial startup: filenames & directories
std::vector<std::string> global_filenames(0);
std::vector<std::string> global_directories(0);

// vector to save threads' exceptions
std::vector<std::exception_ptr> global_exceptions;
std::mutex coutmtx, global_exceptmutex; // mutex for std::cout and for exceptions thrown in threads

// atomics to indicate busy threads, limited atomics to limit checks in loop
#define NR_ATOMICS 8
std::array<std::atomic<int>, NR_ATOMICS> atomic_running_threads;

// vector to save statistics: would be optimal if threads running time is evenly distributed
std::vector<uint64_t> running_times;
std::mutex running_times_mutex;
std::vector<uint64_t> starting_times;
std::mutex starting_times_mutex;

// seach string 
std::string searching_for{};
std::string search_mode{ "find" }; // "find" or "grep"
uint64_t hits{ 0 };

std::string exe_name;

void* memrchr(const void* buf, int c, size_t num)
{
    unsigned char* pMem = (unsigned char*)buf;

    for (;;) {
        if (num-- == 0) {
            return NULL;
        }

        if (*pMem-- == (unsigned char)c) {
            break;
        }

    }

    return (void*)(pMem + 1);

}

class Worker {
private: 
       const int worker_id = -171717;
       std::string start_with_path;
       // smaller means earlier exit on binary files 
       unsigned int buffer_size = 1024 * 2 + 1; //+ 1 for memalign because read_size == buffer_size - 1; for \0 termination
       unsigned int read_size = buffer_size - 1;
       char* buffer;  // move malloc in Worker class, to alloc once.

public:
    Worker(int n, std::string s) : worker_id(n), start_with_path(s) 
    {
        if (worker_id == -42) { // temp worker for startup
            buffer = (char*)malloc(buffer_size);
            int x;
        }
    }
    ~Worker() {
        if (worker_id == -42) {
            free(buffer);
            int x;
        }
    }
    void operator()() {
        try {
            
            buffer = (char*)malloc(buffer_size);
            if (start_with_path != "") {
                tls_path = start_with_path;
                do_linear_descent();
                working();
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2 + worker_id % 7));
                working();
            }
            free(buffer);
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(global_exceptmutex);
            global_exceptions.push_back(std::current_exception());
        }
    }

    int find_or_grep(std::wstring & filename) {
        int hash = -1;

        if (searching_for == "*") {
            hash = 0; // find/list all files
            //std::cout << "We are in * search mode " << std::endl;
        }

        /// <summary>
        ///  new win32 Grep
        /// </summary>
        /// <param name="filename"></param>
        /// <returns></returns>
    
        else if (search_mode == "grep32")
        {
            char* buffer_old = NULL;
            #define SAVED_BUFFER_SIZE 256        // end of last buffer + start of next buffer
            #define SAVED_BUFFER_HALF_SIZE 128   // save HALF_SIZE bytes from the end of the current buffer 
            char saved_buffer[SAVED_BUFFER_SIZE]; // to check for matches overlapping consecutive buffers [...ma][tch...]
            const char* search_string = searching_for.c_str();
            int fd;
            HANDLE hFile = CreateFile(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE)
                return hash;

            

           // printf("**in FILE**: %ws\n", filename.c_str() );

            int check_saved_buffer = 0;
            bool found_string = false;
            DWORD nIn=0;
            
            while (ReadFile(hFile, buffer, read_size, &nIn, NULL) && nIn)
            {
                {std::lock_guard<std::mutex> lock(coutmtx);
                //printf("**in ReadFile**: %ws\n", filename.c_str());
                }
                void* position = NULL;
                position = memchr(buffer, '\0', nIn);
                if (position)
                {
                    
                    //printf("**skipping FILE**: %ws\n", filename.c_str());
                    //std::wcout << "***skipping FILE***" <<  filename << " END"<< std::endl;
                    hash = -1; // skip binary files
                    break;
                }
                else
                {
                    buffer[nIn] = '\0';
                    if (check_saved_buffer)
                    {
                        check_saved_buffer = 0;
                        if ((searching_for.length() > 1) && nIn)
                        {
                            int min = nIn < searching_for.length() - 1 ? nIn : searching_for.length() - 1;
                            memcpy(saved_buffer + SAVED_BUFFER_HALF_SIZE, buffer, min);
                            saved_buffer[SAVED_BUFFER_HALF_SIZE + min] = '\0';
                            char* saved_buffer_start = saved_buffer + SAVED_BUFFER_HALF_SIZE - 1 - (searching_for.length() - 2);
                            char* pos = strstr(saved_buffer_start, search_string);
                            if (pos)
                            {
                                std::lock_guard<std::mutex> lock(coutmtx);
                                hits++;
                                printf("******CORNER CASE******: %ws: %s\n", filename.c_str(), saved_buffer);
                                hash = 272727;
                            }
                        }
                    }

                    if (nIn == read_size && read_size >= SAVED_BUFFER_HALF_SIZE)
                    {
                        // save end of the buffer
                        check_saved_buffer = 1;
                        memcpy(saved_buffer, buffer + read_size - SAVED_BUFFER_HALF_SIZE, SAVED_BUFFER_HALF_SIZE);
                    }

                    char* ptr_to_next_pos = NULL;
                    char* bckp_buffer = buffer;
                    char* pos;
                    while ((pos = strstr(bckp_buffer, search_string))) {
                        if (hash != 272727) {  // keep the 272727 hash to count corner-cases
                            hash = 1;
                        }
                        hits++;
                        int idx_of_string = pos - buffer;
                        void* newline_before = memrchr(buffer, '\n', idx_of_string + 1);  // from buffer to end and reverse!
                        if (!newline_before) {
                            newline_before = buffer - 1;
                        }
                        //printf("file %s nl_before: %.20s\n", filename, (char*)newline_before + 1);
                        int rest = nIn - idx_of_string;
                        void* newline_after = memchr(pos, '\n', rest);
                        if (!newline_after) {
                            newline_after = buffer + nIn;  // CHANGE n was read_size be careful: read reads bufsize-1 bytes
                        }
                        

                        int line_size = (char*)newline_after - (char*)newline_before;
                        char* sptr = (char*)newline_before + 1;
                        int maxlen = line_size - 1;
                        if (maxlen > 80)
                            maxlen = 80;
                        { std::lock_guard<std::mutex> lock(coutmtx);
                        printf("%ws: %.*s\n", filename.c_str(), maxlen, sptr);
                        }
                        
                        ptr_to_next_pos = pos + searching_for.length();
                        if (ptr_to_next_pos < buffer + nIn) {
                            bckp_buffer = ptr_to_next_pos;
                        }
                        else {
                            break;
                        }

                    } // enf of while

                    if (nIn < read_size)
                        break; // saves one read call
                }// end while
            } // end if hFile

            CloseHandle(hFile);
            
        }
        /// 
        ///
        ///  END of new Win32 Grep
        /// 
        /// 

        else if (search_mode == "grepCPP" || search_mode == "grep" ) { // using C++ std
            
            std::ifstream src(filename);
            if (!src.good()) {
                std::wcout << "error with filename: " << filename << std::endl;
            }
            unsigned long long linenr = 0;

            while (src.good()) {
                std::string line;
                std::getline(src, line);
                linenr++;
                std::string::size_type n;

                if ((n = line.find(searching_for)) != std::string::npos) {
                    std::lock_guard<std::mutex> lock(coutmtx);
                    //std::cout << "linenr: " << linenr << " pos: " << n << " in file: " << filename
                    //          << " " << line.substr(0, 79) << std::endl;
                    std::wcout << filename << ": line# " << linenr << ": ";
                    std::cout << line.substr(0, 255) << std::endl;
                    hits++;
                    hash = 1;
                }
            }
            src.close();

        }
        
        else if (search_mode == "grepCPPI" || search_mode == "grepI") { // no binary
            //::_wcslwr_s(argv[1], ::wcslen(argv[1]) + 1);
            //auto locase(name);
            //::_wcslwr_s((wchar_t*)locase.data(), locase.size() + 1);

            std::ifstream src(filename);
            if (!src.good()) {
                std::wcout << "error with filename: " << filename << std::endl;
                //throw std::ios::failure("src is no good: " ); don't throw or else atomic won't be decremented
            }
            unsigned long long linenr = 0;

            while (src.good()) {
                std::string line;
                std::getline(src, line);
                linenr++;
                std::string::size_type n;

                if ((n = line.find('\0')) != std::string::npos) {
                    //std::lock_guard<std::mutex> lock(coutmtx);
                    //std::cout << filename << " is binary" << std::endl;
                    src.close();
                    hash = -1;
                    break;
                }
                n = 0;
                while ((n = line.find(searching_for, n)) != std::string::npos) {
                    n += searching_for.length();
                    std::lock_guard<std::mutex> lock(coutmtx);
                    std::wcout << "found matching file: " << filename << ": line#: " << linenr;
                    std::cout << line.substr(0, 260) << std::endl;
                    hits++;
                    hash = 1;
                }
            }
            src.close();

        }
        else {  // find-mode
            std::wstring & s = filename;
            std::wstring wsearching_for = ConvertUtf8_2_uni(searching_for);
            if (s.find(wsearching_for) != std::string::npos) {
                std::lock_guard<std::mutex> lock(coutmtx);
                std::wcout << "found matching file: " << filename << std::endl;
                hash = hits++;
            }
        }
        
        return hash;
    }


private:

    void working() {
#ifdef DIE_DEBUG
        auto starting_time = std::chrono::high_resolution_clock::now();
        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(starting_time.time_since_epoch()).count();
#endif

#define RETRY_COUNT 0 // a few or a few dozen works equally well; deprecated with atomics
        int retry_count = RETRY_COUNT;
        int dont_wait_forever = 16011; // to eliminate endless waiting at the 'end of the tree':few dirs left

    check_again:
        int proceed_new = 0;

#define NR_OF_ROUNDS 1 // a few or just one round works equally well

        for (int i = 0; proceed_new == 0 && i < NR_OF_ROUNDS * NR_GLOBAL_DIRECTORIES_MUTEXES; i++) {

            int start_with_i = (worker_id + i) % NR_GLOBAL_DIRECTORIES_MUTEXES; // start with own id-vector
            std::lock_guard<std::mutex> lock(global_directories_mutexes[start_with_i]);

            if (!directories_array_of_vectors[start_with_i].empty()) {
                tls_path = directories_array_of_vectors[start_with_i].back();
                directories_array_of_vectors[start_with_i].pop_back();
                proceed_new = 1;
                break;
            }
            
        }
        if (proceed_new == 1) { // equal to if(tls_path != "")
            retry_count = RETRY_COUNT; // reset retry_count
            do_linear_descent();
        }
        else {
            goto end_label; // thread will die here eventually
        }

        goto check_again;   // after returning from do_linear_descent() in if(proceed_new==1) above,
                            // check if there is more work

    end_label:
        if (retry_count-- > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(510));
            goto check_again;
        }
        else {
            for (int i = 0; i < NR_ATOMICS; i++) {
                if (atomic_running_threads[i] > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(517));
                    if (dont_wait_forever-- > 0) { // protection for way too many threads for remaining directories
                                                   // i.e. there are threads running but way more are busy waiting here
                        goto check_again;
                    }
                    else {
                        break;
                    }
                }
            }
#ifdef DIE_DEBUG
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::nanoseconds ns = end_time - starting_time;
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(ns).count();
            { std::lock_guard<std::mutex> lock(coutmtx);
            std::cout << "worker: " << worker_id << " FINALLY DIED after: " << elapsed << " microseconds " << elapsed / 1000 << " millisecs " << elapsed / 1000000 << " secs" << std::endl;
            }

            { std::lock_guard<std::mutex> lock(running_times_mutex);
            running_times.push_back(static_cast<uint64_t>(elapsed));
            }
#endif
            return;
        }

    }

    /*
     descent into ONE subdirectory found by `do_tree_walking()` depth-first,
     meaning: step into ONE directory after another, till the end of this branch of the directory tree
    */

    void do_linear_descent() { // list dir, step into first dir

        // increase atomic counter
        atomic_running_threads[worker_id % NR_ATOMICS]++;

        do {
            do_dir_walking(tls_path);

            // get one directory
            if (!tls_directories.empty()) {
                tls_path = tls_directories.back();
                tls_directories.pop_back();
            }
            else {
                tls_path = ""; // don't walk into same dir again
            }
            // save filenames to global vector specific to this [thread_id % NR_MUTEXES]
            {
                std::lock_guard<std::mutex> lock(global_filename_mutexes[worker_id % NR_GLOBAL_FILENAME_MUTEXES]);
                for (auto const& v : tls_filenames) {
                    filename_array_of_vectors[worker_id % NR_GLOBAL_FILENAME_MUTEXES].emplace_back(v);
                }
            }

            tls_filenames.clear();
            // save remaining N-1 directories to global for other threads to pick up
            {
                std::lock_guard<std::mutex> lock(global_directories_mutexes[worker_id % NR_GLOBAL_DIRECTORIES_MUTEXES]);
                for (auto const& v : tls_directories) {
                    directories_array_of_vectors[worker_id % NR_GLOBAL_DIRECTORIES_MUTEXES].emplace_back(v);
                }
            }
            tls_directories.clear();

        } while (tls_path != "");

        // decrease atomic counter
        atomic_running_threads[worker_id % NR_ATOMICS]--;
    }

    public:
    std::wstring ConvertUtf8_2_uni(const std::string& utf8)
    {
        if (utf8.empty()) return std::wstring{};
        
        std::wstring wstring{};
        CStringW uni;
        int cc = 0;
        // get length (cc) of the new widechar excluding the \0 terminator first
        
        if ((cc = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0) - 1) > 0)
        {
            // convert
            wstring.resize(cc);
            //wchar_t* buf = uni.GetBuffer(cc);
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wstring.data(), cc);
            //uni.ReleaseBuffer();
        }
        return wstring;
    }
    std::string ConvertUni2utf8(const WCHAR* uni)
    {
        std::string s;
        int cc = 0;
        if ((cc = WideCharToMultiByte(CP_UTF8, 0, uni, -1, NULL, 0, 0, 0) - 1) > 0)
        {

            s.resize(cc);
            WideCharToMultiByte(CP_UTF8, 0, uni, -1, s.data(), cc, 0, 0);
        }
        return s;
    }

    private:
    int do_dir_walking(const std::string& sInputPath) {

        std::wstring wInputPath = ConvertUtf8_2_uni(sInputPath + std::string{"\\*"});

        //WCHAR filename[33000];

        WIN32_FIND_DATA w32fd;
        HANDLE hFindFirst = ::FindFirstFileEx(
            //LR"(c:\users\abc\source\myfindwin\x64\debug\*)",                       // lpFileName
            (LPCWSTR)wInputPath.c_str(),
            FindExInfoBasic,            // fInfoLevelId
            &w32fd,                     // lpFindFileData
            FindExSearchNameMatch,      // fSearchOp
            (void*)0,                   // lpSearchFilter
            FIND_FIRST_EX_LARGE_FETCH
        );

        if (hFindFirst == INVALID_HANDLE_VALUE)
        {
            DWORD error = GetLastError();
            

            if ( search_mode == "find" &&   error == 5) // access is denied
            {
                    //tls_filenames.emplace_back(sInputPath + "\\****ACCESS_DENIED****\\"); // put in output.txt
            }
            else
            {
                printf("FindFirstFileEx failed (%d) : %s\n", error, sInputPath.data());
            }

            return 0;
        }
        else
        {
            //std::cout << "handle ok" << std::endl;
            if ( (w32fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) //== FILE_ATTRIBUTE_DIRECTORY )
            {
                std::wstring ww32fdcFileName(w32fd.cFileName);
                if (ww32fdcFileName != L"." && ww32fdcFileName != L"..") {
                //if ( (w32fd.cFileName[0] != L'.' && w32fd.cFileName[1] != L'\0' ) && (w32fd.cFileName[0] != L'.' && w32fd.cFileName[1] != L'.' && w32fd.cFileName[2] != L'\0' )) {
                    //std::lock_guard<std::mutex> lock(coutmtx);
                    //printf("firstDIRECTORY: %ws\n", w32fd.cFileName);
                    std::string PathAsUtf8 = ConvertUni2utf8(w32fd.cFileName);
                    //std::cout << "dir as utf8: " << PathAsUtf8 << std::endl;
                    std::string FullPath = sInputPath + "\\" + PathAsUtf8;
                    //std::cout << "FullPath: " << FullPath << std::endl;
                    tls_directories.emplace_back(FullPath);
                }
                else
                {
                    //std::lock_guard<std::mutex> lock(coutmtx);
                    //std::wcout << "not matching: " << std::wstring(w32fd.cFileName) << std::endl;
                }
            }
            else
            {
                for (int i = 0; i < 22; i++)
                    printf("ERROR ERROR ERRROR - also add FirstFile/Dir\n");
                printf("firstXFile: %ws ALWAYS . as FindFirstFile? \n", w32fd.cFileName);

            }

            while (FindNextFile(hFindFirst, &w32fd))
            {
                
                if ((w32fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) //== FILE_ATTRIBUTE_DIRECTORY)
                {
                    std::wstring ww32fdcFileName(w32fd.cFileName);

                    //if ( ( (w32fd.cFileName[0] != L'.') || (w32fd.cFileName[1] != L'\0') ) && ( (w32fd.cFileName[0] != L'.') || ( w32fd.cFileName[1] != L'.') || (w32fd.cFileName[2] != L'\0') )) {
                    if( ww32fdcFileName != L"." && ww32fdcFileName != L".." ){
                        //std::lock_guard<std::mutex> lock(coutmtx);
                        //printf("DIRECTORY: %ws\n", w32fd.cFileName);
                        std::string PathAsUtf8 = ConvertUni2utf8(w32fd.cFileName);
                        std::string FullPath = sInputPath + "\\" + PathAsUtf8;
                        tls_directories.emplace_back(FullPath);

                        if (searching_for == std::string{ '*' }) {
                            findStreams(FullPath, 1);
                        }
                    }
                    else {
                        //std::lock_guard<std::mutex> lock(coutmtx);
                        //std::wcout << "not matching: " << std::wstring(w32fd.cFileName) << std::endl;
                    }
                }
                else
                {
                   
                    std::string PathAsUtf8 = ConvertUni2utf8(w32fd.cFileName);
                    std::string FullPath = sInputPath + "\\" + PathAsUtf8;

                    if (searching_for == std::string{ '*' }) {
                        tls_filenames.emplace_back(FullPath);
                        
                        findStreams(FullPath, 0);

                    }
                    else {
                        int hash = -1;
                        std::wstring tmp = ConvertUtf8_2_uni(FullPath);
                        hash = find_or_grep( tmp );
                        //std::cout << path_entry.c_str() << std::endl;
                        if (hash != -1) {
                            tls_filenames.emplace_back(FullPath);
                        }
                    }

                }
            }

            FindClose(hFindFirst);
        }
        return 27;

    } // end of do-dir-walking


    void findStreams(std::string & FullPath, bool isFolder) {
        // streams:
        WIN32_FIND_STREAM_DATA wfstreamdata;
        HANDLE streamHandle = FindFirstStreamW(
            ConvertUtf8_2_uni(FullPath).c_str(),
            FindStreamInfoStandard,
            &wfstreamdata,
            0);
        if (streamHandle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == 38)
            {
                if (isFolder == 0) // files without stream are suspicious
                {
                    std::lock_guard<std::mutex> lock(coutmtx);
                    std::cout << "StreamHandle invalid: error: no stream found in: " << FullPath << std::endl;
                }
            }
            else if (GetLastError() == 5)
            {
                // skip Access Denied Error
            }
            else
            {
                std::lock_guard<std::mutex> lock(coutmtx);
                std::cout << "StreamHandle invalid: error: " << GetLastError() << " in: " << FullPath << std::endl;
            }
            return;
        }
        while (streamHandle && GetLastError() != 5 && GetLastError() != 3 && GetLastError() != 2) {
            //std::wcout << "Streamhandle ok: " << wfstreamdata.cStreamName << std::endl;

            std::wstring streamName{ wfstreamdata.cStreamName };
            if (streamName != std::wstring{ L"::$DATA" })
            {
                std::lock_guard<std::mutex> lock(coutmtx);
                
                std::wstring FolderOrFile = isFolder ? std::wstring(L"FOLDER: ") : std::wstring(L"FILE: ");
                LARGE_INTEGER size = wfstreamdata.StreamSize;
                LONGLONG lsize = size.QuadPart;
                std::wstring alertSize = lsize > 1024 ? L" **** ALERT ****" : L"";
                std::wcout << "NON default streamname: " << wfstreamdata.cStreamName << " in " << FolderOrFile << ConvertUtf8_2_uni(FullPath) << " size: " << lsize << alertSize << std::endl;
            }

            if (!FindNextStreamW(streamHandle, &wfstreamdata)) {
                break;
            }
        }
    }

}; // end of class Worker


void do_startup_file_walking(std::string starting_path) {
    auto ec = std::error_code();
	try {
		Worker w(-42, "empty");
		std::filesystem::path startpath(starting_path);
        auto skip = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::directory_iterator dir_iter(startpath, skip, ec);
		std::filesystem::directory_iterator end;

        for(; dir_iter != end; dir_iter.increment(ec) )
        {
            if (ec)
            {
                std::cerr << "EC1: " << ec << startpath << std::endl;
                std::cerr << "EC1: " << ec.message() << std::endl;
                continue;
            }
			std::filesystem::path path = dir_iter->path();
            
			if ((std::filesystem::is_directory(path, ec) || std::filesystem::is_regular_file(path, ec)) && !std::filesystem::is_symlink(path, ec)) {
                if (ec)
                {
                    std::cerr << "EC2: "  << ec << path << std::endl;
                    std::cerr << "EC2: " << ec.message() << std::endl;
                    continue;
                }
          
				std::string path_entry = std::filesystem::canonical(path).string();
				if (std::filesystem::is_directory(path)) {
                    global_directories.emplace_back(path_entry); 
				}
				else if (std::filesystem::is_regular_file(path)) {
                    
                    std::wstring wpath_entry = std::filesystem::canonical(path).wstring();
                    int hash = w.find_or_grep(wpath_entry);

					if (hash != -1) {
                        global_filenames.emplace_back(path_entry);
					}
				}
                
			}
		}
	}
    catch (std::filesystem::filesystem_error const& ex) {
        std::cout
            << "what():  " << ex.what() << '\n'
            << "path1(): " << ex.path1() << '\n'
            << "path2(): " << ex.path2() << '\n'
            << "code().value():    " << ex.code().value() << '\n'
            << "code().message():  " << ex.code().message() << '\n'
            << "code().category(): " << ex.code().category().name() << '\n';

    }
	catch (const std::exception& e) {
		std::cerr << "exception: do_startup_file_walking(): " << e.what() << '\n';
	}
}


int main(int argc, char * argv[]) {
    
    std::locale x("en_US.UTF-8");
    std::locale::global(x);
    _setmaxstdio(2048);

    exe_name = argv[0];

    int n_threads = -1;
    n_threads = std::thread::hardware_concurrency() ;

    if (argc > 1) {
        try {
            n_threads = std::stoi(argv[1]);
        }
        catch (const std::exception& e) {
            std::cerr << "argv1 is no integer in function: " << e.what() << std::endl;
            return -1;
        }
    }

    std::string inPath;
    if (argc >= 4) {
        inPath = std::string(argv[2]);
        std::cout << "We are searching in Directory: " << inPath << std::endl;
    }
    else {
        std::cout << "usage: " << argv[0] << " <number of threads> <path> <search_string> [grep|grepI|find]" << "\ndefault is find; use * as <search_string> to list all files; Output is in output.txt" << std::endl;
        return -1;
    }
    searching_for = argv[3];

    if (argc >= 5) {
        if (std::string{ argv[4] } != "") {
            search_mode = std::string{ argv[4] };
        }
    }
    std::cout << "Searching for: " << searching_for << " in mode " << search_mode;

    if (search_mode == "find")
        std::cout << " NOTE: also searching in file_path part not just after last /"; // more like find -type f <path> | grep string

    std::cout << std::endl;

    std::vector<std::thread> threads;
    std::vector<Worker> workers;
    global_exceptions.clear();

    if (inPath != "") {
        do_startup_file_walking(inPath);
    }
    else {
        //do_startup_file_walking("./");
    }

    int starting_dirs = global_directories.size();

#ifndef PRINTFILENAMES // clean output i.e. just print the filenames 
    std::cout << "starting_dirs: " << starting_dirs << " nthreads " << n_threads << std::endl;
#endif

#ifdef DEBUG2
    std::cout << "global_dirs size: " << starting_dirs << std::endl;
#endif    

#ifdef DEBUG2
    if (starting_dirs < n_threads) {
        std::cout << "less dirs than threads to begin with" << std::endl;
    }
    else {
        std::cout << "more or equal dirs as threads to begin with" << std::endl;
    }
#endif

    for (int i = 0; i < n_threads; ++i) {
        if (starting_dirs < n_threads) {
            if (i < starting_dirs) {
                workers.push_back(Worker(i, global_directories.back())); // same
                global_directories.pop_back();                           // same
            }
            else {
                workers.push_back(Worker(i, ""));
            }
        }
        else {
            workers.push_back(Worker(i, global_directories.back()));     // same
            global_directories.pop_back();                               // same, re-arrange?
        }
    }

    // fill queues
    int tmp = 0;
    while (!global_directories.empty()) {
        directories_array_of_vectors[(tmp++ % n_threads) % NR_GLOBAL_DIRECTORIES_MUTEXES].push_back(global_directories.back());
        global_directories.pop_back();
    }

#ifdef DEBUG2
    for (int i = 0; i < NR_GLOBAL_DIRECTORIES_MUTEXES; i++) {
        std::cout << "directories_array_of_vectors[" << i << "].size(): " << directories_array_of_vectors[i].size() << std::endl;
        for (auto const& v : directories_array_of_vectors[i]) {
            std::cout << "directories_array_of_vectors[" << i << "] = " << v << std::endl;
        }
    }
    std::cout << "starting glob_dirs: residual-size: " << global_directories.size() << std::endl;
    for (auto& v : global_directories) {
        std::cout << "starting glob_dirs: " << v << std::endl;
    }
#endif    
    

    // start threads(Worker)
    for (int i = 0; i < n_threads; ++i) {
        threads.emplace_back(std::ref(workers[i]));
    }

#ifdef DEBUG2     
    std::cout << "in main: workers size: " << workers.size() << std::endl;
    std::cout << "in main: threads size: " << threads.size() << std::endl;
#endif   

    for (auto& v : threads) {
        v.join();
    }

    /* process exceptions from threads */
    for (auto const& e : global_exceptions) {
        try {
            if (e != nullptr) {
                std::rethrow_exception(e);
            }
        }
        catch (std::exception const& ex) {
            std::cerr << "EZZOZ1 exception: "  << std::endl;
            std::cerr << "EZZOZ2 exception: " <<  ex.what() << std::endl;
        }
    }

    if (global_directories.empty()) {
#ifdef DEBUG2        
        std::cout << "global_directories is empty" << std::endl;
#endif
    }
    else {
        std::cout << "global_directories HAS MORE WORK => needs next round of threads!" << std::endl;
        for (auto const& v : global_directories) {
            std::cout << "globdirs-residuals: " << v << std::endl;
        }
    }

    for (int i = 0; i < NR_GLOBAL_DIRECTORIES_MUTEXES; i++) {
        if (!directories_array_of_vectors[i].empty()) {
            std::cout << "MORE WORK: directories_array_of_vectors[" << i << "].size(): " << directories_array_of_vectors[i].size() << std::endl;
            for (auto const& v : directories_array_of_vectors[i]) {
                std::cout << "directories_array_of_vectors[" << i << "] = " << v << std::endl;
            }
        }
    }

#ifdef DEBUG
    std::cout << "Global Filenames: (size: " << global_filenames.size() << ")" << std::endl;
#endif    

    int file_sum = global_filenames.size();
#ifdef PRINTFILENAMES     // clean output i.e. just print the filenames 
    for (auto const& v : global_filenames) {
        std::cout << v << std::endl;
    }
#endif    

    std::cout << "SEARCH HIT COUNT: " << hits << std::endl;
    // write output file with inode,filename,hash


#ifdef OUTPUT    // define in project properties under PREPROCESSOR
    std::ofstream dest("output.txt", std::ios::binary);
    if (!dest)
        throw std::ios::failure(__FILE__ ":" + std::to_string(__LINE__));
    //dest << "inode;file;hash" << std::endl; // write csv header
#endif

    for (int i = 0; i < NR_GLOBAL_FILENAME_MUTEXES; i++) {
       
        //std::cout << "filename_array_of_vectors[" << i << "].size(): " << filename_array_of_vectors[i].size() << std::endl;
      
        if (filename_array_of_vectors[i].size() > 0) {
            file_sum += filename_array_of_vectors[i].size();
        }

        for (auto const& v : filename_array_of_vectors[i]) {
#ifdef PRINTFILENAMES            
            std::cout << v << "\n";
#endif

#ifdef OUTPUT
            dest << v << "\r\n"; // WRITE TO dest == ./output file
#endif
        }
    }
#ifdef OUTPUT
    for (auto const& v : global_filenames) {
        dest << v << "\r\n";
    }
    dest.close();
#endif

#ifdef PRINTFILENAMES
    for (auto const& v : global_filenames) {
        std::cout << v << "\n";
    }
#endif

#ifndef PRINTFILENAMES  // clean output i.e. just print the filenames 
    //std::cout << "filename_array_of_vectors.size(): " << filename_array_of_vectors.size() << std::endl;
    std::cout << "number of files: file_sum: " << file_sum << std::endl;
#endif

    //std::cout << "file size SUM: " << sum_file_size << std::endl;

#ifdef DIE_DEBUG // print threads' elapsed runtime:
    std::sort(running_times.begin(), running_times.end());
    std::cout << "\"running\" times" << std::endl;
    for (auto v : running_times) {
        std::cout << v << std::endl;
    }
    std::cout << "MAX_MIN:" << std::endl;
    auto minmax = std::minmax_element(running_times.begin(), running_times.end());
    // std::make_pair(first, first)
    if (minmax != std::make_pair(running_times.begin(), running_times.begin())) {
        std::cout << "max: " << *minmax.second << " min: " << *minmax.first << std::endl;
    }
    auto dif = *minmax.second - *minmax.first;
    std::cout << "difference(microsecs) between max and min: " << dif << " millisecs: " << dif / 1000 << std::endl;
#endif

} // end int main(int argc, char* argv[])



