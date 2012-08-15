#ifndef DEF_PTHREAD_TOOLS_HPP
#define DEF_PTHREAD_TOOLS_HPP

// Stolen from GraphLab

#include <cstdlib>
#include <memory.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#ifndef WINDOWS
#include <sys/time.h>
#endif

#include <vector>
#include <cassert>
#include <list>
#include <iostream> 

#undef _POSIX_SPIN_LOCKS
#define _POSIX_SPIN_LOCKS -1




/**
 * \file pthread_tools.hpp A collection of utilities for threading
 */
namespace graphchi {
    
    
    
    /**
     * \class mutex 
     * 
     * Wrapper around pthread's mutex On single core systems mutex
     * should be used.  On multicore systems, spinlock should be used.
     */
#ifndef WINDOWS
    class mutex {
    private:
        // mutable not actually needed
        mutable pthread_mutex_t m_mut;
    public:
        mutex() {
            int error = pthread_mutex_init(&m_mut, NULL);
            assert(!error);
        }
        inline void lock() const {
			std::cout << "pthread mutexlock" << std::endl;
            int error = pthread_mutex_lock( &m_mut  );
            assert(!error);
        }
        inline void unlock() const {
            int error = pthread_mutex_unlock( &m_mut );
            assert(!error);
        }
        inline bool try_lock() const {
            return pthread_mutex_trylock( &m_mut ) == 0;
        }
        ~mutex(){
 
		   int error = pthread_mutex_destroy( &m_mut );
           assert(!error);
 
		}
        friend class conditional;
    }; // End of Mutex
    
#else


	// Note: cannot be stack allocated!
    class mutex { // WINDOWS VERSION
    private:
        // mutable not actually needed
        HANDLE ghMutex; 


    public:
        mutex() {
           ghMutex = CreateMutex( 
				NULL,              // default security attributes
				FALSE,             // initially not owned
				NULL);             // unnamed mutex
           assert(ghMutex != NULL);
        }
        void lock() const {
		    DWORD dwWaitResult = WaitForSingleObject( 
            ghMutex,    // handle to mutex
            INFINITE);  // no time-out interval
		   assert(dwWaitResult == WAIT_OBJECT_0);
        }
        void unlock() const {
            if (!ReleaseMutex(ghMutex)) assert(false);
        }
        inline bool try_lock() const {
           assert(false); // not implemented
        }
        ~mutex(){
 
		      CloseHandle(ghMutex);
		}
        friend class conditional;
    }; // End of Mutex
    

#endif

 
#if _POSIX_SPIN_LOCKS >= 0
    // We should change this to use a test for posix_spin_locks eventually
    
    // #ifdef __linux__
    /**
     * \class spinlock
     * 
     * Wrapper around pthread's spinlock On single core systems mutex
     * should be used.  On multicore systems, spinlock should be used.
     * If pthread_spinlock is not available, the spinlock will be
     * typedefed to a mutex
     */
    class spinlock {
    private:
        // mutable not actually needed
        mutable pthread_spinlock_t m_spin;
    public:
        spinlock () {
            int error = pthread_spin_init(&m_spin, PTHREAD_PROCESS_PRIVATE);
            assert(!error);
        }
        
        inline void lock() const { 
            int error = pthread_spin_lock( &m_spin  );
            assert(!error);
        }
        inline void unlock() const {
            int error = pthread_spin_unlock( &m_spin );
            assert(!error);
        }
        inline bool try_lock() const {
            return pthread_spin_trylock( &m_spin ) == 0;
        }
        ~spinlock(){
            int error = pthread_spin_destroy( &m_spin );
            assert(!error);
        }
        friend class conditional;
    }; // End of spinlock
#define SPINLOCK_SUPPORTED 1
#else
    //! if spinlock not supported, it is typedef it to a mutex.
    typedef mutex spinlock;
#define SPINLOCK_SUPPORTED 0
#endif
    
    
    
  
         
    
    
#define atomic_xadd(P, V) __sync_fetch_and_add((P), (V))
#define cmpxchg(P, O, N) __sync_val_compare_and_swap((P), (O), (N))
#define atomic_inc(P) __sync_add_and_fetch((P), 1)
    
  
} // end namespace
#endif
