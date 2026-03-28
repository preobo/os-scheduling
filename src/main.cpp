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
#include <algorithm>

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
void readyProcess(Process* p, SchedulerData* data);
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
            readyProcess(p, shared_data);
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
        uint64_t curr_time = currentTime();
       
        //   - *Check if all processes are terminated, and if so set shared variable to end scheduling threads  
        //assume all terminated
        shared_data->all_terminated = true; 
        for(Process* p: processes){
            if(p->getState() != Process::State::Terminated){
                shared_data->all_terminated = false;
                break;
            }
        }        


        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        for(Process* p: processes){
            switch (p->getState())
            {
            case Process::State::NotStarted:
            //checking if processes should be launched
                if(curr_time - start >= p->getStartTime())
                //if so, change state to ready and add to queue
                {p->setState(Process::State::Ready, curr_time);
                readyProcess(p, shared_data);
                }
                break;
            
            case Process::State::Ready:
            //check each process, if state changes to ready, add to queue
                if(p->isInterrupted())
                {
                    p->interruptHandled();
                    readyProcess(p, shared_data);
                }
                break;
            case Process::State::IO:
             //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
                p->updateProcess(currentTime());
                //if burst is done, update state to ready and add to queue
                if(p->getState() == Process::State::Ready){
                    //if so, change state to ready and add to queue
                    readyProcess(p, shared_data);
                }
                break;
            
            case Process::State::Running:
             //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
                switch(shared_data->algorithm)
                {
                    //check if time sleice expired
                    case ScheduleAlgorithm:: RR:
                        if(currentTime() - p->getBurstStartTime() >= shared_data->time_slice)
                        {
                            p->interrupt();
                            p->updateProcess(currentTime());
                            //if process is still ready after update, add back to queue
                            if(p->getState() == Process::State::Ready)
                            {
                                readyProcess(p, shared_data);
                                p->interruptHandled();
                            }
                        }
                        break;
                    case ScheduleAlgorithm::PP:
                        // -*Check for ready process with higher priority
                        shared_data->queue_mutex.lock();
                        if(!shared_data->ready_queue.empty() && shared_data->ready_queue.front()->getPriority() < p->getPriority())
                        {
                            shared_data->queue_mutex.unlock();
                            p->interrupt();
                            p->updateProcess(currentTime());
                            if(p->getState() == Process::State::Ready)
                            {
                                readyProcess(p,shared_data);
                                p->interruptHandled();
                            }
                        }
                        else{
                            shared_data->queue_mutex.unlock();
                        }
                        break;  
                    default:
                        break;
                    }
                    break;
                //check if process is terminated, if so, free up core and update state
                case Process::State::Terminated:
                    break;
                }
            }
            //print progress
            printProcessOutput(processes);
            
            //sleep 50 ms
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            //clear output
            erase();
        }

    
    // wait for threads to finish
    for(i = 0; i < num_cores; i++)
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

    //compute final statistics
    double total_cpu_time = 0;
    double total_turnaround = 0;
    double total_wait = 0;
    uint64_t end_sim = currentTime();
    double sim_duration = (end_sim - start) / 1000.0;

    //average processes finished in first 50% and second 50%
    std::vector<Process*> sorted = processes;
    std::sort(sorted.begin(), sorted.end(), [](Process* a, Process* b){
        return a->getTurnaroundTime() < b->getTurnaroundTime();
    });

    int n = sorted.size();
    double throughput_first = 0.0;
    double throughput_second = 0.0;
    int half = n / 2;

    //compute total cpu time, turnaround time, wait time, and throughput for first and second half of processes
    for(int i = 0; i < n; i++)
    {
        total_cpu_time += sorted[i]->getCpuTime();
        total_turnaround += sorted[i]->getTurnaroundTime();
        total_wait += sorted[i]->getWaitTime();
        if(i < half)
        {
            throughput_first += 1.0 / sorted[i]->getTurnaroundTime();
        }
        else
        {
            throughput_second += 1.0 / sorted[i]->getTurnaroundTime();
        }
    }
    endwin();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{

    while (!(shared_data->all_terminated))
    {
        // this pointer is passed from main(locking the mutex manually)
        std::unique_lock<std::mutex> lock(shared_data->queue_mutex);
        // now we have the lock, so we can safely access shared data (ready queue)
        Process *p = nullptr;

        //if ready queue is not empty 
        if(!shared_data->ready_queue.empty())
        {
            //get the process at the front of ready queue
            p = shared_data->ready_queue.front();
            //pop the process from the ready queue
            shared_data->ready_queue.pop_front();
            //unlock mutex so other's can access
            lock.unlock();

            p->setState(Process::State::Running, currentTime());
            p->setCpuCore(core_id);
            p->setBurstStartTime(currentTime());
        }

        if(p != nullptr)
        {
            //load context switch time
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));
            //check if the queue is empty, if so, wait and check again
            while(p->getState() == Process::State::Running)
            {
                //simulate the processes running
                p->updateProcess(currentTime());
                //sleep for 5ms 
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            //save context switch save time
            std::this_thread::sleep_for(std::chrono::milliseconds(shared_data->context_switch));

            //free up the core 
            p = nullptr;
        }
        else
        {
            //queue is empty, check after 5ms
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void readyProcess(Process* p, SchedulerData* shared_data)
{
    std::lock_guard<std::mutex> lock(shared_data->queue_mutex);
    std::list<Process*>::iterator it;
    
    switch(shared_data->algorithm)
    {   
        //for shortest job first, loop over queue and insert when remaining time is less
        case ScheduleAlgorithm::SJF:
            for(it = shared_data->ready_queue.begin(); it != shared_data->ready_queue.end(); ++it)
            {
                if(p->getRemainingTime() < (*it)->getRemainingTime())
                {
                    break;
                }
            }
            shared_data->ready_queue.insert(it, p);
            break;
        //for preemptive priority, insert when priority is higher
        case ScheduleAlgorithm::PP:
            for(it = shared_data->ready_queue.begin(); it != shared_data->ready_queue.end(); ++it)
            {
                if(p->getPriority() > (*it)->getPriority())
                {
                    break;
                }
            }
            shared_data->ready_queue.insert(it, p);
            break;
        default:
            shared_data->ready_queue.push_back(p);
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
