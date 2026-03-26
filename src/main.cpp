#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <ncurses.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex queue_mutex;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
void printProcessOutput(std::vector<Process*>& processes);
std::string makeProgressString(double percent, uint32_t width);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char *argv[])
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data = new SchedulerData();
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = scr::readConfigFile(argv[1]);

    // Store number of cores in local variable for future access
    uint8_t num_cores = config->cores;

    // Store configuration parameters in shared data object
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    scr::deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    initscr();
    while (!(shared_data->all_terminated))
    {
        // Do the following:
        //   - Get current time
        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        //     - NOTE: ensure processes are inserted into the ready queue at the proper position based on algorithm
        //   - Determine if all processes are in the terminated state
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization

        // Maybe simply print progress bar for all procs?
        printProcessOutput(processes);

        // sleep 50 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // clear outout
        erase();
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics (use `printw()` for each print, and `refresh()` after all prints)
    //  - CPU utilization
    //  - Throughput
    //     - Average for first 50% of processes finished
    //     - Average for second 50% of processes finished
    //     - Overall average
    //  - Average turnaround time
    //  - Average waiting time


    // Clean up before quitting program
    processes.clear();
    endwin();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // this pointer is passed from main(locking the mutex manually)
    std::unique_lock<std::mutex> lock(shared_data->queue_mutex);
    // now we have the lock, so we can safely access shared data (ready queue)
    Process *p = nullptr;

    //if ready queue is not empty 
    if(!shared_data->ready_queue.empty()){
        //get the process at the front of ready queue
        p = shared_data->ready_queue.front();
        //pop the process from the ready queue
        shared_data->ready_queue.pop_front();
        p->setState(Process::State::Running, currentTime());
        p->setCpuCore(core_id);
        //unlock mutex so other's can access
        lock.unlock();
        //sleep for context switch load time
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(p != nullptr){
    //load context switch time
        std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));
    //check if the queue is empty, if so, wait and check again
        while(p->getState() == Process::State::Running){
            //simulate the processes running 
            p->updateProcess(5);
            //sleep for 5ms 
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        //save context switch save time
        std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));

        //free up the core 
        p = nullptr;
    }
    else{
        //queue is empty, check after 5ms
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void printProcessOutput(std::vector<Process*>& processes)
{
    printw("|   PID | Priority |    State    | Core |               Progress               |\n"); // 36 chars for prog
    printw("+-------+----------+-------------+------+--------------------------------------+\n");
    for (int i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double total_time = processes[i]->getTotalRunTime();
            double completed_time = total_time - processes[i]->getRemainingTime();
            std::string progress = makeProgressString(completed_time / total_time, 36);
            printw("| %5u | %8u | %11s | %4s | %36s |\n", pid, priority,
                   process_state.c_str(), cpu_core.c_str(), progress.c_str());
        }
    }
    refresh();
}

std::string makeProgressString(double percent, uint32_t width)
{
    uint32_t n_chars = percent * width;
    std::string progress_bar(n_chars, '#');
    progress_bar.resize(width, ' ');
    return progress_bar;
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
