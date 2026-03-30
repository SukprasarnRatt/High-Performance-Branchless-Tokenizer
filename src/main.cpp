#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numa.h>
#include <numaif.h>
#include <queue>
#include <sched.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "tokenizer.hpp"

using namespace std;
namespace fs = std::filesystem;

// Global mutex used to prevent multiple threads from writing to std::cout
std::mutex cout_mutex;

// Stores one loaded file in memory.
// - filePath: original path of the file
// - content: vector holding the allocated file buffer(s)
// - size: number of bytes in the file
struct FileData {
    string filePath;
    vector<char*> content;
    size_t size;
};

/*
 * Recursively scans the input directory and collects all regular files.
 *
 * Returns:
 *   A vector of (file path, file size) pairs.
 *
 * Purpose:
 *   This function builds the list of files that will later be distributed
 *   across NUMA nodes and processed by worker threads.
 */
static vector<pair<string, uintmax_t>> crawlDataset(const string& path) {
    vector<pair<string, uintmax_t>> fileInfos;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            try {
                uintmax_t fileSize = entry.file_size();
                fileInfos.emplace_back(entry.path().string(), fileSize);
            } catch (const fs::filesystem_error& e) {
                cerr << "Error getting size of file: " << entry.path()
                     << " - " << e.what() << endl;
            }
        }
    } catch (const std::exception& e) {
        cerr << "Filesystem error: " << e.what() << endl;
    }

    return fileInfos;
}

/*
 * Pins the current thread to CPUs that belong to a specific NUMA node.
 *
 * Parameters:
 *   threadId - logical ID used only for logging
 *   nodeId   - NUMA node to bind the thread to
 *   label    - descriptive thread label for output messages
 *
 * Purpose:
 *   NUMA-aware execution can improve memory locality by keeping a thread
 *   running on CPUs that are close to the memory it is expected to use.
 */
static void setThreadAffinityToNode(int threadId, int nodeId, const string& label) {

    // Create a CPU set object used for thread affinity.
    cpu_set_t cpuset;

    // Clear the CPU set so no CPUs are selected yet.
    CPU_ZERO(&cpuset);

    // cpumask is a pointer to a NUMA-library bitmask structure.
    // The bitmask is large enough to represent all possible CPUs on the system.
    // It is used to ask the NUMA library which CPUs belong to a specific NUMA node.
    // For thread affinity.
    struct bitmask* cpumask = numa_bitmask_alloc(numa_num_possible_cpus());

    if (cpumask == nullptr) {
        lock_guard<mutex> guard(cout_mutex);
        cerr << "Error allocating NUMA bitmask for " << label << " " << threadId << endl;
        return;
    }


    // ask the NUMA library to fill cpumask with the CPUs that belong to NUMA node nodeId;
    // writes the answer into cpumask
    if (numa_node_to_cpus(nodeId, cpumask) != 0) {
        lock_guard<mutex> guard(cout_mutex);
        cerr << "Error retrieving CPUs for node " << nodeId << endl;
        numa_bitmask_free(cpumask);
        return;
    }

    // look through every CPU position in cpumask; if that CPU belongs to the NUMA node, add it to Linux CPU set
    for (int i = 0; i < cpumask->size; ++i) {
        if (numa_bitmask_isbitset(cpumask, i)) {
            CPU_SET(i, &cpuset);
        }
    }

    numa_bitmask_free(cpumask);

    // tell the operating system which CPUs this thread is allowed to run on
    // 0 mean the thread currently executing this function.
    int ret = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    {
        lock_guard<mutex> guard(cout_mutex);
        if (ret != 0) {
            cerr << "Error setting thread affinity for " << label << " "
                 << threadId << " on node " << nodeId << endl;
        } else {
            cout << label << " " << threadId
                 << " affinity set to Node " << nodeId << endl;
        }
    }

    // Check which logical CPU this thread is currently running on,
    // then get the NUMA node for that CPU to verify affinity behavior.
    int current_cpu = sched_getcpu();
    int current_node = numa_node_of_cpu(current_cpu);

    {
        lock_guard<mutex> guard(cout_mutex);
        cout << label << " " << threadId
             << " is running on CPU " << current_cpu
             << " (Node " << current_node << ")" << endl;
    }
}

static void loadFilesOnNode(
    int threadId,
    int nodeId,
    const vector<pair<string, uintmax_t>>& files,
    queue<FileData>& fileBuffer,
    mutex& bufferMutex,
    bool affinityFlag
) {

    // if NUMA affinity is enabled, pin this loader thread to CPUs belonging to nodeId
    if (affinityFlag) {
        setThreadAffinityToNode(threadId, nodeId, "Loader Thread");
    }

    for (const auto& [filePath, fileSize] : files) {
        if (filePath.empty() || filePath.find("/.") != string::npos || fileSize == 0) {
            continue;
        }
        // Open file and filePath.c_str() converts C++ string to C string style
        // Because the Linux open() function is a C system call interface, and it expects the file path as a C-style string
        int fd = open(filePath.c_str(), O_RDONLY);
        if (fd == -1) {
            lock_guard<mutex> guard(cout_mutex);
            cerr << "Loader Thread " << threadId
                 << " - Error opening file: " << filePath << endl;
            continue;
        }

        // Allocate memory for the file. +1 for \0
        char* buffer = new char[fileSize + 1];

        // Read file contents into memory
        // read() returns the number of bytes read, 0 for EOF, or -1 on error.
        // ssize_t is signed integer for byte count and etc.
        ssize_t bytesRead = read(fd, buffer, fileSize);

        // checks whether the whole file was read successfully.
        if (bytesRead == static_cast<ssize_t>(fileSize)) {
            buffer[fileSize] = '\0';

            // Done with reading the file
            // Tell kernel to remove this file from memory (hint)
            posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            close(fd);

            // Create a vector to hold the buffer pointer
            vector<char*> bufferVector;
            bufferVector.push_back(buffer);


            // Lock the queue and push the loaded file into fileBuffer
            {
                lock_guard<mutex> lock(bufferMutex);
                fileBuffer.push({filePath, std::move(bufferVector), static_cast<size_t>(fileSize)});
            }
        } else {
            lock_guard<mutex> guard(cout_mutex);
            cerr << "Loader Thread " << threadId
                 << " - Error reading file: " << filePath << endl;
            delete[] buffer;
            close(fd);
        }
    }

    lock_guard<mutex> guard(cout_mutex);
    cout << "Loader Thread " << threadId
         << " completed loading files on Node " << nodeId << endl;
}

// worker thread function doing Tokenization.
static void processFiles(
    int threadId,
    vector<queue<FileData>>& fileBuffersPerNode,
    vector<mutex>& bufferMutexes,
    mutex& tokenMutex,
    mutex& bytesMutex,
    char charDict[256],
    uintmax_t& totalBytes,
    uintmax_t& totalTokens,
    vector<double>& tokenizationTimes,
    vector<uintmax_t>& bytesProcessed,
    bool affinityFlag
) {

    // Check total number of Numa node
    int totalNodes = numa_max_node() + 1;
    if (totalNodes <= 0) {
        totalNodes = 1;
    }


    // Assign this thread to a NUMA node using round-robin scheduling.
    int node = (threadId - 1) % totalNodes;

    // If affinity is enabled, bind the thread to CPUs belonging to that node.
    if (affinityFlag) {
        setThreadAffinityToNode(threadId, node, "Thread");
    }

    double threadTokenizationTime = 0.0;

    // Process loaded files until this thread's assigned queue becomes empty.
    while (true) {
        FileData fileData;
        bool foundWork = false;
        
        // Repeatedly pull one loaded file from this node's queue and process it.
        // Stop when there is no more work left in the queue.
        {
            lock_guard<mutex> lock(bufferMutexes[node]);
            if (!fileBuffersPerNode[node].empty()) {
                fileData = std::move(fileBuffersPerNode[node].front());
                fileBuffersPerNode[node].pop();
                foundWork = true;
            }
        }

        if (!foundWork) {
            break;
        }

        uintmax_t fileSize = fileData.size;

        {
            lock_guard<mutex> lock(bytesMutex);
            totalBytes += fileSize;
        }
        bytesProcessed[threadId - 1] += fileSize;

        char* buffer = fileData.content[0];

        auto tokenStart = chrono::high_resolution_clock::now();
        vector<char*> tokens = tokenize(buffer, fileSize, charDict);
        auto tokenEnd = chrono::high_resolution_clock::now();

        chrono::duration<double> tokenDuration = tokenEnd - tokenStart;
        threadTokenizationTime += tokenDuration.count();

        {
            lock_guard<mutex> lock(tokenMutex);
            totalTokens += tokens.size();
        }

        delete[] buffer;

        tokenizationTimes[threadId - 1] = threadTokenizationTime;
    }
}

int main(int argc, char* argv[]) {

    // Expect:
    //   argv[1] = input directory
    //   argv[2] = optional number of worker threads
    //   argv[3] = optional affinity flag (0 = off, 1 = on)
    if (argc < 2 || argc > 4) {
        cerr << "Usage: ./tokenizer <input_directory> [num_threads] [affinity_flag]" << endl;
        cerr << "Example: ./tokenizer /path/to/data 16 1" << endl;
        return 1;
    }

    string directoryPath = argv[1];

    
    if (!fs::exists(directoryPath) || !fs::is_directory(directoryPath)) {
        cerr << "Error: invalid directory path " << directoryPath << endl;
        return 1;
    }

    // By default, use the number of hardware threads available on the machine.
    // If hardware_concurrency() cannot detect it, fall back to 1 threads.
    int numThreads = static_cast<int>(thread::hardware_concurrency());
    if (numThreads <= 0) {
        numThreads = 1;
    }

    // If the user provided a thread count, override the default value.
    if (argc >= 3) {
        numThreads = stoi(argv[2]);
        if (numThreads <= 0) {
            cerr << "Error: num_threads must be > 0" << endl;
            return 1;
        }
    }


    // Optional NUMA affinity flag:
    //   0 = do not bind threads to NUMA nodes
    //   1 = bind threads to NUMA nodes
    bool affinityFlag = false;
    if (argc >= 1) {
        affinityFlag = (stoi(argv[3]) != 0);
    }

    cout << "Starting indexFiles with path: " << directoryPath << endl;

    // Shared counters for total processed bytes and total discovered tokens.
    uintmax_t totalBytes = 0;
    uintmax_t totalTokens = 0;

    // Scan the dataset and collect file paths with their sizes.
    vector<pair<string, uintmax_t>> fileInfos = crawlDataset(directoryPath);
    cout << "Crawled dataset. Number of files: " << fileInfos.size() << endl;

    // Sort files from largest to smallest to distribute large files more evenly
    // across NUMA nodes.
    sort(fileInfos.begin(), fileInfos.end(),
         [](const auto& a, const auto& b) { return a.second > b.second; });

    // Detect how many NUMA nodes are available on the machine.
    int totalNodes = numa_max_node() + 1;
    if (totalNodes <= 0) {
        cerr << "NUMA is not available or could not determine the number of nodes." << endl;
        totalNodes = 1;
    }
    cout << "Total NUMA nodes detected: " << totalNodes << endl;


    // Split the file list across NUMA nodes in round-robin order.
    // Each node gets its own group of files.
    vector<vector<pair<string, uintmax_t>>> filesPerNode(totalNodes);
    for (size_t i = 0; i < fileInfos.size(); ++i) {
        int node = static_cast<int>(i % totalNodes);
        filesPerNode[node].push_back(fileInfos[i]);
    }

    // Each NUMA node has:
    //   - one queue holding files already loaded into memory
    //   - one mutex protecting that queue
    vector<queue<FileData>> fileBuffersPerNode(totalNodes);
    vector<mutex> bufferMutexes(totalNodes);


    // Launch one loader thread per NUMA node.
    // Each loader reads its assigned files from disk into memory.
    vector<thread> loaderThreads;
    for (int node = 0; node < totalNodes; ++node) {
        loaderThreads.emplace_back(
            loadFilesOnNode,
            node + 1,
            node,
            cref(filesPerNode[node]),
            ref(fileBuffersPerNode[node]),
            ref(bufferMutexes[node]),
            affinityFlag
        );
    }

    // Wait for all loader threads to finish before starting tokenization.
    for (auto& t : loaderThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    // Count how many files were successfully loaded into the per-node queues.
    size_t totalFilesLoaded = 0;
    for (const auto& q : fileBuffersPerNode) {
        totalFilesLoaded += q.size();
    }
    cout << "All loader threads have completed. Total files loaded: "
         << totalFilesLoaded << endl;

    
    // Build the 256-entry lookup table used by the tokenizer.
    // Alphanumeric characters are marked as valid token characters,
    // and delimiters are marked as 0.
    char charDict[256];
    loadDictionaryAlphaNumeric(charDict);
    // Mutexes protecting shared counters updated by worker threads.
    mutex tokenMutex, bytesMutex;

    // Store per-thread tokenization times and processed byte counts.
    vector<double> tokenizationTimes(numThreads, 0.0);
    vector<uintmax_t> bytesProcessed(numThreads, 0);

    // Start timing the processing stage.
    auto totalStart = chrono::high_resolution_clock::now();

    // Launch processing threads.
    // Each worker pulls loaded files from its assigned NUMA-node queue
    // and tokenizes them.
    vector<thread> processingThreads;
    for (int i = 0; i < numThreads; ++i) {
        processingThreads.emplace_back(
            processFiles,
            i + 1,
            ref(fileBuffersPerNode),
            ref(bufferMutexes),
            ref(tokenMutex),
            ref(bytesMutex),
            charDict,
            ref(totalBytes),
            ref(totalTokens),
            ref(tokenizationTimes),
            ref(bytesProcessed),
            affinityFlag
        );
    }
    // Wait for all processing threads to finish.
    for (auto& t : processingThreads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // Stop timing after all worker threads complete.
    auto totalEnd = chrono::high_resolution_clock::now();
    chrono::duration<double> totalDuration = totalEnd - totalStart;
    double totalTime = totalDuration.count();
    // Find the thread that spent the longest time tokenizing files.
    int longestThreadId = 0;
    double longestTime = 0.0;

    for (int i = 0; i < numThreads; ++i) {
        if (tokenizationTimes[i] > longestTime) {
            longestTime = tokenizationTimes[i];
            longestThreadId = i + 1;
        }
    }

    cout << fixed << setprecision(4);
    // Print per-thread timing and byte statistics.
    for (int i = 0; i < numThreads; ++i) {
        cout << "Thread " << (i + 1)
             << " tokenization time: " << tokenizationTimes[i]
             << " seconds" << endl;

        cout << "Thread " << (i + 1)
             << " processed " << bytesProcessed[i]
             << " bytes" << endl;
    }

    cout << "Thread " << longestThreadId
         << " took the longest time for tokenization: "
         << longestTime << " seconds" << endl;

    cout << "Total execution time (create and join threads): "
         << totalTime << " seconds" << endl;
    // Sum bytes processed by all worker threads.
    uintmax_t totalProcessedBytes = 0;
    for (int i = 0; i < numThreads; ++i) {
        totalProcessedBytes += bytesProcessed[i];
    }

    cout << "Completed indexing " << totalProcessedBytes << " bytes of data" << endl;
    cout << "Completed indexing " << totalTokens << " tokens" << endl;
     // Compute throughput in MB/s for the processing stage.
    double throughput_MB_per_s = 0.0;
    if (totalTime > 0.0) {
        throughput_MB_per_s =
            (static_cast<double>(totalProcessedBytes) / (1024.0 * 1024.0)) / totalTime;
    }

    cout << "Average Throughput: " << throughput_MB_per_s << " MB/s" << endl;

    return 0;
}