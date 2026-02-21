# Foundry Runtime
Foundry Runtime is a collection of runtime tools and performance experiments focused on microarchitectural behavior and thread synchronization across multiple layers of abstraction.  

## SPSC Lock-Free Queue
This is a single-producer, single-consumer lock-free queue implementation. It currently only works for trivially copyable types (proof of concept), but I am currently working on making it valid for all types. The API is quite simple and is intended to be used by two separate threads (consumer and producer).  
  
It is defined as follows:  
```
foundry_runtime::spsc_queue<class T, size_t capacity = 128, bool enable_cacheline_padding = false, size_t prefetch_distance = 0>
```
`T` is the type contained in the queue. Currently it is only valid for trivially copyable types and will generate a compiler error otherwise.  
  
`capacity` is the size of the ring-buffer (just an array of type T) that the queue is implemented as. `capacity` must be a power of 2.  
  
`enable_cacheline_padding` is a performance optimization that stores the atomic read and write (enqueue + dequeue) indices into a struct that is aligned to the size of a cacheline (128 bytes for Apple M-Series computers). This struct is then padded with a char buffer that is never referenced to the length of the cacheline not occupied by the atomic index. As a result, we don't get cache invalidate ping-pong between the two cores running the producer and consumer threads (especially when the read and write indices would share the same cacheline otherwise). 
   
`prefetch_distance` is another performance optimization that uses software read and write prefetch instructions to hint the CPU to pull entries in the ring buffer into the L1 cache of the core running the thread that will soon need them before a read or write occurs. This reduces CPU stalls while the core goes and fetches the line (can be 100s of wasted cycles if not more). The distance ahead in the queue (array) that will be prefetched can be modulated by this template parameter. 0 disables prefetch. Any number beyond this specifies how many entries ahead we prefetch. For example, if we have a uint64_t and specify a `prefetch_distance` of 4, then we will be prefetching 32 bytes ahead (on M3 a quarter-line).  
  

### SPSC Queue Performance Experiments
Although I definitely have some utility for this, I used this as an exercise to explore how these various optimizations vary in performance with various ring buffer capacities. I also compared performance to generic mutex wrapped `std::queue`. I ran 10 million enqueue and dequeue operations 10 times per queue size and optimization permutation and averaged the result into one entry in the plot below. These were all run on my Apple M3 Max with `QoS=USER_INTERACTIVE` to bias towards p-cores, although MacOS provides no user APIs I am aware of that allows me explicitly schedule a thread to be run on a p-core. It is additionally worth noting that I compiled with the -O3 clang compiler flag and that the producer/consumer threads yield on a failure to enqueue or dequeue although experiments with busy waiting did not meaningfully change performance.  
  
![SPSC Optimization Data](examples/spsc_queue/figures/spsc_data.png)
The results are quite interesting and illustrate some interesting microarchitectural performance regimes. I will lay out some of my observations and interpretations below. 
1. **Mutex Queue vs Non-Optimized Atomic Implementation**    
a. **Lock-Free Throughput Maxima**   
    The lock-free queue of `uint64_t` without prefetch and r/w index padding outperforms a standard mutex queue between ~16-512 entries (128 B to 4 KB). In this range, the entire ring buffer fits easily within each core's 128 KB L1d and is thus not bound by capacity and L2/L3 latency. The mutex implementation uses costly lock/unlock operations, as well as additional branching to serialize execution independent of cacheline contention. The lock-free implementation instead uses atomic r/w index updates. Despite significant traffic between cores, the lock-free queue achieves ~2x throughput in this range (really just ~32-256).  
    At small sizes (~4-16 entries), the producer and consumer repeatedly operate on the same cachelines in tight sequence. Each enqueue requires exclusive line ownership with subsequent reads from the consumer. The lines are invalidated and ownership is transferred between cores so frequently that the queue stalls, throttling throughput.  
    For larger queue sizes (>~512 entries), although they are all relatively small/fit entirely in the L1d, the reuse distance increases. As a result these lines are less likely to remain hot in the producer's L1d. Without software prefetch, enqueue operations (queue writes) see read for ownership (RFO) latency begin to dominate causing the throughput to collapse as the queue becomes bound by L2/L3 latency (RFOs resolved higher up in the hierarchy). At higher buffer sizes, throughput collapses and ownership latency exceeds mutex latency.  
    The lock-free queue transitions from coherence-limited, to RFO latency limited as ring size increases, finding a local maximum between ~16-512 entries, while the mutex based queue is synchronization limited independent of size.   
b. **Worst Case Synchronization Mechanics Between Queue Implementations**    
    Beyond the latency considerations of RFO and L1d cache hit frequency, the primary difference between the lock-free queue and mutex implementation is synchronization mechanics. Worst case, the lock-free queue remains entirely in EL0 userspace relying on load acquire and store release memory ordering ops. Alternatively, the mutex implementation may fall back on the kernel when under contention invoking `ulock` wait and wake calls, transitioning to and from EL1 kernelspace.  
    To illustrate this, I inspected the ARM assembly generated by each implementation and followed the mutex branches to the stdlib dylibs provided by MacOS Developer Tools (if relevant), that I disassembled. To keep this short(er), I only contrast the enqueue functionality as the dequeue is quite similar and would be redundant.  
    `try_enqueue(T&...)` for the lock-free implementation disassembles into the following paths (I follow the branches and focus in on the relevant instructions in the stdlib atomic implementation):  
    ```
    auto current_write_loc = write_next.r_w_index.load(std::memory_order_relaxed); 
    // lowers to 
    ldr x?, [x?]   

    cached_read_loc = read_next.r_w_index.load(std::memory_order_acquire); 
    // lowers to
    ldapr  x?, [x?]   

    write_next.r_w_index.store(next_loc, std::memory_order_release); 
    // lowers to
    stlr   x?, [x?]    
    ```
    Each strongly ordered atomic load and store to the read and write indices, we set up a very low cost acquire/release instruction pair. Worst case, synchronization is all EL0 with no `dmb` or `dsb` instructions, and no transitions to the kernel (EL1) with some minor branching overhead.  
    `try_enqueue(T&...)` for the mutex based worst case has a considerably higher worst case cost. Although uncontended locks tend to stay in user space, under contention the implementation may take a slow path that results in multiple kernel entries.
    ```
    std::unique_lock<std::mutex> lock(mut);
    // contentious case translates into a combination of the following calls

    __ulock_wait 
    // lowers to 
    mov    x16, #0x203 ; =515 // syscall 515
    svc    #0x80              // EL0 -> EL1 (kernel)

    __ulock_wake
    mov    x16, #0x204 ; =516 // syscall 516
    svc    #0x80              // EL0 -> EL1 (kernel)
    ```
    In the contentious case, the control flow for a single queue write can look like:  
    ```
    Thread A (EL0)
    -> try_enqueue(T&...)
    -> fails to acquire the mutex
    -> __ulock_wait
    -> EL0 -> EL1 (Thread A "goes to sleep")

    Thread B (EL0)
    -> releases the mutex
    -> __ulock_wake
    -> EL0 -> EL1 ("wakes up" Thread A)

    Thread A 
    -> resumes execution in EL0
    -> writes to the queue
    ```
    A single write to the queue can involve multiple transitions to the kernel, wheras the lock-free implementation always remains entirely in EL0 relying only on acquire/release instructions and cache coherency.  

    Kernel transitions can be extremely expensive, but that alone doesn't guarantee better performance. For very small and larger buffer sizes, the unoptimized lock-free implementation performs worse than the mutex-based queue despite avoiding syscalls. This indicates that microarchitecture, not just kernel transitions, drastically affects performance. To address this, I used additional techniques to improve baseline throughput and stabilize across buffer sizes. I outline these below.  
    
2. **Atomic Index Cacheline Padding+Alignment Optimization**    
This is labelled as `Pad=On` on the queue configuraiton axis. Its purpose is to eliminate false sharing between the ring buffer read and write indices. To achieve this, the atomic read and write inidices are aligned to the size of a cacheline (128 B on M-Series Macs and 64 B on most x86 systems). Each index is padded to completely occupy its own cacheline to prevent unintended interation. 
    ```
    // explicitly set to 128 bytes on my system as this is not included in the MacOS clang installation
    static constexpr std::size_t cacheline_size = std::hardware_destructive_interference_size;

    // declare the following type that is aligned to the size of a cacheline, and is also exactly the size of a cacheline
    struct alignas(cacheline_size) PaddedLine {
        std::atomic<std::size_t> r_w_index{0}; // the actual atomic member we care about
        char pad[cacheline_size - sizeof(std::atomic<std::size_t>)]{}; // empty padding
    };

    // declare queue read and write indices with the PaddedLine type
    PaddedLine write_next{}; 
    PaddedLine read_next{}; 
    ```
    Without padding, `write_next` and `read_next` are declared adjacently, so in most cases, share the same cacheline. Although the producer only modifies `write_next` and the consumer only modifies `read_next`, the CPU handles coherence on a cacheline basis. If both indices occupy the same cacheline, we go through a continuous loop of:  
    -> Produce enqueues and writes `write_next`    
    -> Cacheline marked dirty in the producer's core   
    -> The consumers copy of that line gets invalidated even though `read_next` was not modified      
    -> Consumer dequeues and wants to write `read_next`
    -> Consumer core must go out and fetch that line exclusive
    -> Consumer writes `read_next`       
    -> We do this for every enqueue/dequeue pair...    
    Although the variables are independent, the entire cacheline must get moved between cores. This results in a large amount of unintended line ownership transfers. Padding forces `write_next` and `read_next` onto their own cachelines allowing each core to hold exclusive write ownership of that line. The additonal memory footprint is negligible compared to the costly ownership transfer in the alternative.
    The effects of this optimization are shown in the plot above. The `Pad=On Prefetch=Off` curve has a similar shape to `Pad=Off Prefetch=Off`, but its local maximum between ~256-512 entries has ~3x higher throughput. At very small buffer sizes, performance is similar to the unoptimized version because the primary performance cost is ownership transfer of actual entries in the ring buffer as opposed to the read/write indices. At larger buffer sizes, reuse distance increases and L2/L3 latency again becomes the limiter. False sharing of the indices does not prevent throughput collapse, and performance converges with the unoptimized verison. Cacheline padding alone significantly improves throughput in a very specific window, but it does not address L2/L3 latency associated with larger memory footprint. These experiments were only run with `uint64_t` which is relatively small. I sought a way to stablize across larger memory footprints, both in terms of buffer size and type size.  


**Prefetch Optimization Analysis IN PROGRESS**

  
![Prefetch Experiment](examples/spsc_queue/figures/prefetch_experiment.png)