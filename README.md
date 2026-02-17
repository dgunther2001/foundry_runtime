# Foundry Runtime
This is a collection of various runtime tools as well as a personal exploration/proving ground. I will add descriptions to this doc as the tools are written and provide APIs/a usage guide.  

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
1. The lock-free queue of `uint64_t` without prefetch and r/w index padding outperforms a standard mutex queue between ~16-512 entries (128 B to 4 KB). In this range, the entire ring buffer fits easily within each core's 128 KB L1d and is thus not bound by capacity and L2/L3 latency. The mutex implementation adds memory fences, lock/unlock operations, as well as additional branching to serialize execution independent of cacheline contention. The lock-free implementation instead uses atomic r/w index updates. Despite significant traffic between cores, the lock-free queue achieves ~2x throughput in this range (really just ~32-256).  
At small sizes (~4-16 entries), the producer and consumer repeatedly operate on the same cachelines in tight sequence. Each enqueue requires exclusive line ownership with subsequent reads from the consumer. The lines are invalidated and ownership is transferred between cores so frequently that the queue stalls, throttling throughput.  
For larger queue sizes (>~512 entries), although they are all relatively small/fit entirely in the L1d, the reuse distance increases. As a result these lines are less likely to remain hot in the producer's L1d. Without software prefetch, enqueue operations (queue writes) see read for ownership (RFO) latency begin to dominate causing the throughput to collapse as the queue becomes bound by L2/L3 latency (RFOs resolved higher up in the hierarchy). At higher buffer sizes, throughput collapses and ownership latency exceeds mutex latency.  
The lock-free queue transitions from coherence-limited, to RFO latency limited as ring size increases, finding a local maximum between ~16-512 entries, while the mutex based queue is synchronization limited independent of size.  
**MORE TO COME/WIP**

  
![Prefetch Experiment](examples/spsc_queue/figures/prefetch_experiment.png)