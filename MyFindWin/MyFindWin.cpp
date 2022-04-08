/*
 ============== my find grep: concurrent find respectively concurrent grep ==============

usage:
.\MyFindWin.exe <number of threads> <path> <search_string> [find|grepCPP|grepCPPI]  # defaults to find

    use * as <search_string> to list all files

    Program will save found files in: "output.txt"

- find: C++ string search on whole file path (i.e. find xyz will match both: c:\Users\xyz\Desktop\xyz.txt)
- grepCPP: using C++ searches binary files
- grepCPPI: using C++ skipping binary files (like grep -I)

e.g. ./mfg 64 /usr/src/linux  "MAINTAINERS" grep

notes:

grep by default skips binary files (files with \0 in them)

also searching in file path part not just after last  more like find -type f <path> | grep <search_string>

*/

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

// global directory vectors & mutexes
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


class Worker {
private: 
        const int worker_id = -171717;
       std::string start_with_path;
       // smaller means earlier exit on binary files 
       unsigned int buffer_size = 1024 * 2 + 1; //+ 1 for memalign because read_size == buffer_size - 1; for \0 termination
       unsigned int read_size = buffer_size - 1;
       char* buffer;  // move malloc in Worker class, to alloc once.

public:
    Worker(int n, std::string s) : worker_id(n), start_with_path(s) {
        if (worker_id == -42) { // temp worker for startup
            //buffer = (char*)malloc(buffer_size);
            int x;
        }
    }
    ~Worker() {
        if (worker_id == -42) {
            //free(buffer);
            int x;
        }
    }
    void operator()() {
        try {
            
            //buffer = (char*)malloc(buffer_size);
            if (start_with_path != "") {
                tls_path = start_with_path;
                do_linear_descent();
                working();
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2 + worker_id % 7));
                working();
            }
            //free(buffer);
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(global_exceptmutex);
            global_exceptions.push_back(std::current_exception());
        }
    }

    int find_or_grep(std::string filename) {
        int hash = -1;

        if (searching_for == "*") {
            hash = 0; // find/list all files
            //std::cout << "We are in * search mode " << std::endl;
        }

        else if (search_mode == "grepCPP" || search_mode == "grep" ) { // using C++ std
            std::ifstream src(filename);
            if (!src.good()) {
                std::cout << "error with filename: " << filename << std::endl;
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
                    std::cout << filename << ": " << line.substr(0, 255) << std::endl;
                    hits++;
                    hash = 1;
                }
            }
            src.close();

        }
        else if (search_mode == "grepCPPI" || search_mode == "grepI") { // no binary
            std::ifstream src(filename);
            if (!src.good()) {
                std::cout << "error with filename: " << filename << std::endl;
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
                    std::cout << filename << ": " << line.substr(0, 79) << std::endl;
                    hits++;
                    hash = 1;
                }
            }
            src.close();

        }
        else {  // find-mode
            std::string s = filename;
            if (s.find(searching_for) != std::string::npos) {
                std::lock_guard<std::mutex> lock(coutmtx);
                std::cout << "" << filename << std::endl;
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
        int dont_wait_forever = 16; // to eliminate endless waiting at the 'end of the tree':few dirs left

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
            // save N-1 directories to global for other threads to pick up
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

    int do_dir_walking(const std::string& sInputPath) {

        auto ec = std::error_code();
        try {

            std::filesystem::path path(sInputPath);
            //path = std::filesystem::canonical(path);

            std::filesystem::directory_iterator dir_iter(path, ec);
            //std::filesystem::directory_options::skip_permission_denied);
            std::filesystem::directory_iterator end;

            for (; dir_iter != end; dir_iter.increment(ec))
            {
                if (ec)
                {
                    std::cout << "EC1 worker: " << ec << sInputPath <<  std::endl;
                    std::cout << "EC1 worker: " << ec.message() << std::endl;
                    continue;
                }

                std::filesystem::path path = dir_iter->path();
                
                if ((std::filesystem::is_directory(path, ec) || std::filesystem::is_regular_file(path, ec)) || std::filesystem::is_symlink(path, ec)) {

                    if (ec)
                    {
                        std::cout << "EC1 worker: " << ec << path << std::endl;
                        std::cout << "EC1 worker: " << ec.message() << std::endl;
                        continue;
                    }
                    
                    std::string path_entry = path.string();

                    //std::cout << "current: " << path_entry << std::endl;
                    if (std::filesystem::is_directory(path, ec) && !std::filesystem::is_symlink(path, ec)) {
                        tls_directories.emplace_back(path_entry + "\\");
                    }
                    
                    else if (std::filesystem::is_symlink(path, ec)) {
                        std::cout << "XXXX: FOUND symlink on WINDOWS!?!?: " << path << std::endl;
                    }
                    else if (std::filesystem::is_regular_file(path, ec)) {
                        
                        int hash = -1;
                        hash = find_or_grep(path_entry);
                        //std::cout << path_entry.c_str() << std::endl;
                        if (hash != -1) {
                            tls_filenames.emplace_back(path_entry);
                        }
                    }
                }
                
            } // end while dir iteration

    }
        catch (std::system_error const& ex) {
            std::cout << "EXCPTN:system_error "<< ex.code().message() << ex.code() << ex.what() << std::endl;
        
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
            std::cerr << "exception: do_dir_walking(): " << e.what() << '\n';
            std::cerr << "Type:    " << typeid(e).name() << "\n";
        }
        return 0;
    } // end of do-dir-walking

}; // end of class Worker


void do_startup_file_walking(std::string starting_path) {
    auto ec = std::error_code();
	try {
		Worker w(-42, "empty");
		std::filesystem::path startpath(starting_path);
        auto skip = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::directory_iterator dir_iter(startpath, skip, ec);
		std::filesystem::directory_iterator end;

		//while (dir_iter != end) {
        for(; dir_iter != end; dir_iter.increment(ec) )
        {
            if (ec)
            {
                std::cerr << "EC1: " << ec << startpath << std::endl;
                std::cerr << "EC1: " << ec.message() << std::endl;
                continue;
            }
            //std::filesystem::path path = dir_e.path();
			std::filesystem::path path = dir_iter->path();
            //std::cout << "PATH: " << path << std::endl;
            
			if ((std::filesystem::is_directory(path, ec) || std::filesystem::is_regular_file(path, ec)) && !std::filesystem::is_symlink(path, ec)) {
                if (ec)
                {
                    std::cerr << "EC2: "  << ec << path << std::endl;
                    std::cerr << "EC2: " << ec.message() << std::endl;
                    continue;
                }
          
				std::string path_entry = std::filesystem::canonical(path).string();
				if (std::filesystem::is_directory(path)) {
                    //std::cout << "DIRECTORY: " << path << std::endl;
                    global_directories.emplace_back(path_entry); // + "/");
				}
				else if (std::filesystem::is_regular_file(path)) {
                    //std::cout << path << std::endl;
					int hash = w.find_or_grep(path_entry);
                    
					if (hash != -1) {
                        global_filenames.emplace_back(path_entry); // +";" + std::to_string(hash));
					}
				}
                
			}
            
			//dir_iter.increment(ec);
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
	//catch (const std::exception& e) {
		//std::cerr << "exception: do_startup_file_walking(): " << e.what() << '\n';
       // std::cerr << "exception: e.msg: " << ec.message() <<  std::endl;
	//}
}

int main(int argc, char* argv[]) {
    
    std::locale x("en_US.UTF-8");
    std::locale::global(x);

    

    
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
        //do_startup_file_walking(std::wstring(L"D:\\tmp"));
        //do_startup_file_walking(std::wstring(L"D:\\"));
    }
    else {
        //do_startup_file_walking("./");
    }

    int starting_dirs = global_directories.size();

#ifndef PRINTFILENAMES // clean output i.e. just print the filenames 
    std::cout << "starting_dirs: " << starting_dirs << " nthreads " << n_threads << std::endl;
#endif

   // std::cout << "Exiting HERE" << std::endl;
   // return 1;

#ifdef DEBUG
    std::cout << "global_dirs size: " << starting_dirs << std::endl;
#endif    

#ifdef DEBUG
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

#ifdef DEBUG
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

#ifdef DEBUG     
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
            std::cerr << "EZZOZ exception: " << ex.what() << std::endl;
        }
    }

    if (global_directories.empty()) {
#ifdef DEBUG        
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
            dest << v << "\n"; // WRITE TO dest == ./output file
#endif
        }
    }
#ifdef OUTPUT
    for (auto const& v : global_filenames) {
        dest << v << "\n";
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



