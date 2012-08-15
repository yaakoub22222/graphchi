
/**
 * @file
 * @author  Aapo Kyrola <akyrola@cs.cmu.edu>
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2012] [Aapo Kyrola, Guy Blelloch, Carlos Guestrin / Carnegie Mellon University]
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 
 *
 * @section DESCRIPTION
 *
 * I/O manager.
 */



#ifndef DEF_STRIPEDIO_HPP
#define DEF_STRIPEDIO_HPP

#include <iostream> 

#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <windows.h>
//#include <omp.h>

#include <vector>
#include <deque>
#include <queue>

#include "logger/logger.hpp"
#include "metrics/metrics.hpp"
#include "util/ioutil.hpp"
#include "util/cmdopts.hpp"



namespace graphchi {
    
    static size_t get_filesize(std::string filename);
    struct pinned_file;
    
    /**
     * Defines a striped file access.
     */
    struct io_descriptor {
        std::string filename;    
        std::vector<FILE *> readdescs;
        std::vector<FILE *> writedescs;
        pinned_file * pinned_to_memory;
        int start_mplex;
        bool open;
    };
    
    
    enum BLOCK_ACTION { READ, WRITE };
    
    // Very simple ref count system
    struct refcountptr {
        char * ptr;
        volatile int count;
        refcountptr(char * ptr, int count) : ptr(ptr), count(count) {}
    };
    
    // Forward declaration
    class stripedio;
    
    struct iotask {
        BLOCK_ACTION action;
        FILE * fd;
        
        refcountptr * ptr;
        size_t length;
        size_t offset;
        size_t ptroffset;
        bool free_after;
        stripedio * iomgr;
        
        
        iotask() : action(READ), fd(NULL), ptr(NULL), length(0), offset(0), ptroffset(0), free_after(false), iomgr(NULL) {}
        iotask(stripedio * iomgr, BLOCK_ACTION act, FILE * fd,  refcountptr * ptr, size_t length, size_t offset, size_t ptroffset, bool free_after=false) : 
        action(act), fd(fd), ptr(ptr),length(length), offset(offset), ptroffset(ptroffset), free_after(free_after), iomgr(iomgr) {}
    };

	  
        //template <class T>
        class synchronized_queue_iotask {
            
        public:
            synchronized_queue_iotask() { };
            ~synchronized_queue_iotask() { };
            
            void push(const iotask &item) {
                _queuelock.lock();
                _queue.push(item);
                _queuelock.unlock();
            }
            
            bool safepop(iotask * ret) {
                _queuelock.lock();
                if (_queue.size() == 0) {
                    _queuelock.unlock();
                    
                    return false;
                }
                *ret = _queue.front();
                _queue.pop();
                _queuelock.unlock();
                return true;
            }
            
            iotask pop() {
                _queuelock.lock();
                iotask t = _queue.front();
                _queue.pop();
                _queuelock.unlock();
                return t;
            }
            
            size_t size() const{
                return _queue.size();
            }
        private:
			typedef std::deque<iotask, std::allocator<iotask> > TDEQUE;
			typedef std::queue<iotask,TDEQUE> TQUEUE;


            TQUEUE _queue;
            mutex _queuelock;
        };
        
    
    struct thrinfo {
        synchronized_queue_iotask * readqueue;
        synchronized_queue_iotask * commitqueue;
        synchronized_queue_iotask * prioqueue;
        
        bool running;
        metrics * m;
        volatile int pending_writes;
        volatile int pending_reads;
        int mplex;
    };
    
    // Forward declaration
    static void * io_thread_loop(void * _info);
    
    struct stripe_chunk {
        int mplex_thread;
        size_t offset;
        size_t len;
        stripe_chunk(int mplex_thread, size_t offset, size_t len) : mplex_thread(mplex_thread), offset(offset), len(len) {}
    };
    
    
    struct streaming_task {
        stripedio * iomgr;
        int session;
        size_t len;
        volatile size_t curpos;
        char ** buf;
        streaming_task() {}
        streaming_task(stripedio * iomgr, int session, size_t len, char ** buf) : iomgr(iomgr), session(session), len(len), curpos(0), buf(buf) {}
    };
    
    struct pinned_file {
        std::string filename;
        size_t length;
        uint8_t * data;
        bool touched;
    };
    
    // Forward declaration
    static void * stream_read_loop(void * _info);    
    
    
    class stripedio {
        
        std::vector<io_descriptor *> sessions;
        mutex mlock;
        int blocksize;
        int stripesize;
        int multiplex;
        std::string multiplex_root;
        bool disable_preloading;
        
        std::vector< synchronized_queue_iotask> mplex_readtasks;
        std::vector< synchronized_queue_iotask> mplex_writetasks;
        std::vector< synchronized_queue_iotask> mplex_priotasks;
        std::vector< pthread_t > threads;
        std::vector< thrinfo * > thread_infos;
        metrics &m;
        
        /* Memory-pinned files */
        std::vector<pinned_file *> preloaded_files;
        mutex preload_lock;
        size_t preloaded_bytes;
        size_t max_preload_bytes;
        
        
        int niothreads; // threads per mplex
        
    public:
        stripedio( metrics &_m) : m(_m) {
            disable_preloading = false;
            blocksize = get_option_int("io.blocksize", 1024 * 1024);
            stripesize = get_option_int("io.stripesize", blocksize/2);
            preloaded_bytes = 0;
            max_preload_bytes = 1024 * 1024 * get_option_int("preload.max_megabytes", 0);
            
            if (max_preload_bytes > 0) {
            //    logstream(LOG_INFO) << "Preloading maximum " << max_preload_bytes << " bytes." << std::endl;
            }
            
            multiplex = get_option_int("multiplex", 1);
            if (multiplex>1) {
                multiplex_root = get_option_string("multiplex_root", "<not-set>");
            } else {
                multiplex_root = "";
                stripesize = 1024*1024*1024;
            }
            m.set("stripesize", (size_t)stripesize);
            
            // Start threads (niothreads is now threads per multiplex)
            niothreads = get_option_int("niothreads", 1);
            m.set("niothreads", (size_t)niothreads);
            
            // Each multiplex partition has its own queues
            for(int i=0; i<multiplex * niothreads; i++) {
                mplex_readtasks.push_back(synchronized_queue_iotask());
                mplex_writetasks.push_back(synchronized_queue_iotask());
                mplex_priotasks.push_back(synchronized_queue_iotask());
            }
            
            int k = 0;
            for(int i=0; i < multiplex; i++) {
                for(int j=0; j < niothreads; j++) {
                    thrinfo * cthreadinfo = new thrinfo();
                    cthreadinfo->commitqueue = &mplex_writetasks[k];
                    cthreadinfo->readqueue = &mplex_readtasks[k];
                    cthreadinfo->prioqueue = &mplex_priotasks[k];
                    cthreadinfo->running = true;
                    cthreadinfo->pending_writes = 0;
                    cthreadinfo->pending_reads = 0;
                    cthreadinfo->mplex = i;
                    cthreadinfo->m = &m;
                    thread_infos.push_back(cthreadinfo);
                    
                    pthread_t iothread;
                    int ret = pthread_create(&iothread, NULL, io_thread_loop, cthreadinfo);
                    threads.push_back(iothread);
                    assert(ret>=0);
                    k++;
                }
            }
        }
        
        ~stripedio() {
            int mplex = (int) thread_infos.size();
            // Quit all threads
            for(int i=0; i<mplex; i++) {
                thread_infos[i]->running=false;
            }
            size_t nthreads = threads.size();
            for(unsigned int i=0; i<nthreads; i++) {
                pthread_join(threads[i], NULL);
            }
            for(int i=0; i<mplex; i++) {
                delete thread_infos[i];
            }
            
            for(int j=0; j<(int)sessions.size(); j++) {
                if (sessions[j] != NULL) {
                    delete sessions[j];
                    sessions[j] = NULL;
                }
            }
            
            for(std::vector<pinned_file *>::iterator it=preloaded_files.begin(); 
                it != preloaded_files.end(); ++it) {
                pinned_file * preloaded = (*it);
                delete preloaded->data;
                delete preloaded;
            }
        }
        
        void set_disable_preloading(bool b) {
            disable_preloading = b;
            if (b) logstream(LOG_INFO) << "Disabled preloading." << std::endl;
        }
        
        bool multiplexed() {
            return multiplex>1;
        }
        
        void print_session(int session) {
            for(int i=0; i<multiplex; i++) {
                std::cout << "multiplex: " << multiplex << std::endl;
                std::cout << "Read desc: " << sessions[session]->readdescs[i] << std::endl;
            }
            
            for(int i=0; i<(int)sessions[session]->writedescs.size(); i++) {
                std::cout << "multiplex: " << multiplex << std::endl;
                std::cout << "Read desc: " << sessions[session]->writedescs[i] << std::endl;
            }
        }
        
        // Compute a hash for filename which is used for
        // permuting the stripes. It is important the permutation
        // is same regardless of when the file is opened.
        int hash(std::string filename) {
            const char * cstr = filename.c_str();
            int hash = 1;
            int l = (int) strlen(cstr);
            for(int i=0; i<l; i++) {
                hash = 31*hash + cstr[i];
            }
            return std::abs(hash);
        }
        
        int open_session(std::string filename, bool readonly=false) {
            mlock.lock();
            // FIXME: known memory leak: sessions table is never shrunk
            int session_id = (int) sessions.size();
            io_descriptor * iodesc = new io_descriptor();
            iodesc->open = true;
            iodesc->pinned_to_memory = is_preloaded(filename);
            iodesc->start_mplex = hash(filename) % multiplex;
            sessions.push_back(iodesc);
            mlock.unlock();
            
            if (NULL != iodesc->pinned_to_memory) {
                logstream(LOG_INFO) << "Opened preloaded session: " << filename << std::endl;
                return session_id;
            }
            
            for(int i=0; i<multiplex; i++) {
                std::string fname = multiplexprefix(i) + filename;
                for(int j=0; j<niothreads+(multiplex == 1 ? 1 : 0); j++) { // Hack to have one fd for synchronous
                    FILE * rddesc = fopen(fname.c_str(), (readonly ? "rb" : "r+b"));
                    if (rddesc == NULL)logstream(LOG_ERROR)  << "Could not open: " << fname << " session: " << session_id 
                        << " error: " << strerror(errno) << std::endl;
                    assert(rddesc != NULL);
                    iodesc->readdescs.push_back(rddesc);
 
                    if (!readonly) {
                        FILE * wrdesc = rddesc; // Can use same one

                        if (wrdesc == NULL) logstream(LOG_ERROR)  << "Could not open for writing: " << fname << " session: " << session_id
                            << " error: " << strerror(errno) << std::endl;
                        assert(wrdesc != NULL);
 
                        iodesc->writedescs.push_back(wrdesc);
                    }
                }
            }
            iodesc->filename = filename;
            if (iodesc->writedescs.size() > 0)  {
                logstream(LOG_INFO) << "Opened write-session: " << session_id << "(" << iodesc->writedescs[0] << ") for " << filename << std::endl;
            } else {
                logstream(LOG_INFO) << "Opened read-session: " << session_id << "(" << iodesc->readdescs[0] << ") for " << filename << std::endl;

            }
            return session_id;
        }
        
        void close_session(int session) {
            mlock.lock();
            // Note: currently io-descriptors are left into the vertex array
            // in purpose to make managed memory work. Should be fixed as this is 
            // a (relatively minor) memory leak.
            bool wasopen;
            io_descriptor * iodesc = sessions[session];
            wasopen = iodesc->open;
            iodesc->open = false;
            mlock.unlock();
            if (wasopen) {
                for(std::vector<FILE *>::iterator it=iodesc->readdescs.begin(); it!=iodesc->readdescs.end(); ++it) {
                    fclose(*it);
                }
            }
        }
        
        int mplex_for_offset(int session, size_t off) {
            return ((int) (off / stripesize) + sessions[session]->start_mplex) % multiplex;
        }
        
        // Returns vector of <mplex, offset> 
        std::vector< stripe_chunk > stripe_offsets(int session, size_t nbytes, size_t off) {
            size_t end = off+nbytes;
            size_t idx = off;
            size_t bufoff = 0;
            std::vector<stripe_chunk> stripelist;
            while(idx<end) {
                size_t blockoff = idx % stripesize;
                size_t blocklen = min(stripesize-blockoff, end-idx);
                
                int mplex_thread = (int) mplex_for_offset(session, idx) * niothreads + (int) (random() % niothreads);
                stripelist.push_back(stripe_chunk(mplex_thread, bufoff, blocklen));
                
                bufoff += blocklen;
                idx += blocklen;
            }
            return stripelist;
        }
        
        template <typename T>
        void preada_async(int session,  T * tbuf, size_t nbytes, size_t off) {
            std::vector<stripe_chunk> stripelist = stripe_offsets(session, nbytes, off);
            refcountptr * refptr = new refcountptr((char*)tbuf, (int)stripelist.size());
            for(int i=0; i<(int)stripelist.size(); i++) {
                stripe_chunk chunk = stripelist[i];
                InterlockedIncrement((volatile LONG*) &thread_infos[chunk.mplex_thread]->pending_reads);
                mplex_readtasks[chunk.mplex_thread].push(iotask(this, READ, sessions[session]->readdescs[chunk.mplex_thread], 
                                                                refptr, chunk.len, chunk.offset+off, chunk.offset));
            }
        }
        
        /* Used for pipelined read */
        void launch_stream_reader(streaming_task  * task) {
            pthread_t t;
            int ret = pthread_create(&t, NULL, stream_read_loop, (void*)task);
            assert(ret>=0);
        }
        
        
        /**
         * Pinned sessions process files that are permanently
         * pinned to memory.
         */
        bool pinned_session(int session) {
            return sessions[session]->pinned_to_memory != NULL;
        }
        
        /**
         * Call to allow files to be preloaded. Note: using this requires
         * that all files are accessed with same path. This is true if
         * standard chifilenames.hpp -given filenames are used.
         */
        void allow_preloading(std::string filename) {
            return;
        }
        
        void commit_preloaded() {
           // Do nothing (disabled on windows)
#ifndef WINDOWS
            assert(false);
#endif
        }
        
        pinned_file * is_preloaded(std::string filename) {
            return false;
        }
        
        
        // Note: data is freed after write!
        template <typename T>
        void pwritea_async(int session, T * tbuf, size_t nbytes, size_t off, bool free_after) {
            std::vector<stripe_chunk> stripelist = stripe_offsets(session, nbytes, off);
            refcountptr * refptr = new refcountptr((char*)tbuf, (int) stripelist.size());
            for(int i=0; i<(int)stripelist.size(); i++) {
                stripe_chunk chunk = stripelist[i];
                InterlockedIncrement((volatile LONG*) &thread_infos[chunk.mplex_thread]->pending_writes);
                mplex_writetasks[chunk.mplex_thread].push(iotask(this, WRITE, sessions[session]->writedescs[chunk.mplex_thread], 
                                                                 refptr, chunk.len, chunk.offset+off, chunk.offset, free_after));
            }
        }
        
        template <typename T>
        void preada_now(int session,  T * tbuf, size_t nbytes, size_t off) {
            metrics_entry me = m.start_time();
            
            if (multiplex > 1) {
                std::vector<stripe_chunk> stripelist = stripe_offsets(session, nbytes, off);
                size_t checklen=0;
                refcountptr * refptr = new refcountptr((char*)tbuf, (int) stripelist.size());
                refptr->count++; // Take a reference so we can spin on it
                for(int i=0; i < (int)stripelist.size(); i++) {
                    stripe_chunk chunk = stripelist[i];
                    InterlockedIncrement((volatile LONG*) &thread_infos[chunk.mplex_thread]->pending_reads);
                    
                    // Use prioritized task queue
                    mplex_priotasks[chunk.mplex_thread].push(iotask(this, READ, sessions[session]->readdescs[chunk.mplex_thread], 
                                                                    refptr, chunk.len, chunk.offset+off, chunk.offset));
                    checklen += chunk.len;
                }
                assert(checklen == nbytes);
                
                // Spin
                while(refptr->count>1) {
                    Sleep(5);
                }
                delete refptr;
            } else {
                preada(sessions[session]->readdescs[threads.size()], tbuf, nbytes, off);
            }
            m.stop_time(me, "preada_now", false);
        }
        
        template <typename T>
        void pwritea_now(int session, T * tbuf, size_t nbytes, size_t off) {
            metrics_entry me = m.start_time();
            std::vector<stripe_chunk> stripelist = stripe_offsets(session, nbytes, off);
            size_t checklen=0;
            
            for(int i=0; i<(int)stripelist.size(); i++) {
                stripe_chunk chunk = stripelist[i];
                pwritea(sessions[session]->writedescs[chunk.mplex_thread], (char*)tbuf+chunk.offset, chunk.len, chunk.offset+off);
                checklen += chunk.len;
            }
            assert(checklen == nbytes);
            m.stop_time(me, "pwritea_now", false);
            
        }
        
        
        
        /** 
         * Memory managed versino of the I/O functions. 
         */
        
        template <typename T>
        void managed_pwritea_async(int session, T ** tbuf, size_t nbytes, size_t off, bool free_after) {
            if (!pinned_session(session)) {
                pwritea_async(session, *tbuf, nbytes, off, free_after);
            } else {
                // Do nothing but mark the descriptor as 'dirty'
                sessions[session]->pinned_to_memory->touched = true;
            }
        }
        
        template <typename T>
        void managed_preada_now(int session,  T ** tbuf, size_t nbytes, size_t off) {
            if (!pinned_session(session)) {
                preada_now(session, *tbuf, nbytes,  off);
            } else {
                io_descriptor * iodesc = sessions[session];
                *tbuf = (T*) (iodesc->pinned_to_memory->data + off);
            }
        }
        
        template <typename T>
        void managed_pwritea_now(int session, T ** tbuf, size_t nbytes, size_t off) {
            if (!pinned_session(session)) {
                pwritea_now(session, *tbuf, nbytes, off);
            } else {
                // Do nothing but mark the descriptor as 'dirty'
                sessions[session]->pinned_to_memory->touched = true;
            }
        }
        
        template<typename T>
        void managed_malloc(int session, T ** tbuf, size_t nbytes, size_t noff) {
            if (!pinned_session(session)) {
                *tbuf = (T*) malloc(nbytes);
            } else {
                io_descriptor * iodesc = sessions[session];
                *tbuf = (T*) (iodesc->pinned_to_memory->data + noff);
            }
        }
        
        template <typename T>
        void managed_preada_async(int session, T ** tbuf, size_t nbytes, size_t off) {
            if (!pinned_session(session)) {
                preada_async(session, *tbuf, nbytes,  off);
            } else {
                io_descriptor * iodesc = sessions[session];
                *tbuf = (T*) (iodesc->pinned_to_memory->data + off);
            }
        }
        
        template <typename T>
        void managed_release(int session, T ** ptr) {
            if (!pinned_session(session)) {
                assert(*ptr != NULL);
                free(*ptr);
            }
            *ptr = NULL;
        }
        
        
        void truncate(int session, size_t nbytes) {
            assert(!pinned_session(session));
            assert(multiplex <= 1);  // We do not support truncating on multiplex yet
            int stat = ftruncate(fileno(sessions[session]->writedescs[0]), nbytes); 
            if (stat != 0) {
                logstream(LOG_ERROR) << "Could not truncate " << sessions[session]->filename <<
                " error: " << strerror(errno) << std::endl;
                assert(false);
            }
        }
        
        void wait_for_reads() {
            metrics_entry me = m.start_time();
            int loops = 0;
            int mplex = (int) thread_infos.size();
            for(int i=0; i<mplex; i++) {
                while(thread_infos[i]->pending_reads > 0) {
                    Sleep(10);
                    loops++;
                }
            }
            m.stop_time(me, "stripedio_wait_for_reads", false);
        }
        
        void wait_for_writes() {
            metrics_entry me = m.start_time();
            int mplex = (int) thread_infos.size();
            for(int i=0; i<mplex; i++) {
                while(thread_infos[i]->pending_writes>0) {
                    Sleep(10);
                }
            }
            m.stop_time(me, "stripedio_wait_for_writes", false);
        }
        
        
        std::string multiplexprefix(int stripe) {
            if (multiplex > 1) {
                char mstr[255];
                sprintf(mstr, "%d/", 1+stripe%multiplex);
                return multiplex_root + std::string(mstr);
            } else return "";
        }
        
        std::string multiplexprefix_random() {
            return multiplexprefix((int)random() % multiplex);
        }
    };
    
    
    static void * io_thread_loop(void * _info) {
        iotask task;
        thrinfo * info = (thrinfo*)_info;
        logstream(LOG_INFO) << "Thread for multiplex :" << info->mplex << " starting." << std::endl;
        while(info->running) {
            bool success;
            if (info->pending_reads>0) {  // Prioritize read queue
                success = info->prioqueue->safepop(&task);
                if (!success) {
                    success = info->readqueue->safepop(&task);
                }
            } else {
                success = info->commitqueue->safepop(&task);
            }
            if (success) {
                if (task.action == WRITE) {  // Write
                    metrics_entry me = info->m->start_time();
                    
                    pwritea(task.fd, task.ptr->ptr + task.ptroffset, task.length, task.offset);  
                    if (task.free_after) {
                        // Threead-safe method of memory managment - ugly!
                        if (InterlockedDecrement((volatile LONG*)  &task.ptr->count) == 0) {
                            free(task.ptr->ptr);
                            free(task.ptr);
                        }
                    }
                    InterlockedDecrement((volatile LONG*) &info->pending_writes);
                    info->m->stop_time(me, "commit_thr");
                } else {
                    preada(task.fd, task.ptr->ptr+task.ptroffset, task.length, task.offset); 
                    InterlockedIncrement((volatile LONG*) &info->pending_reads);
                    if (InterlockedDecrement((volatile LONG*) &task.ptr->count) == 0) {
                        free(task.ptr);
                    }
                }
            } else {
                Sleep(50); // 50 ms
            }
        }
        return NULL;
    }
    
    
    static void * stream_read_loop(void * _info) {
        streaming_task * task = (streaming_task*)_info;
        size_t bufsize = 32*1024*1024; // 32 megs
        char * tbuf;
        
        /**
         * If this is not pinned, we just malloc the
         * buffer. Otherwise - shuold just return pointer
         * to the in-memory file buffer.
         */
        if (task->iomgr->pinned_session(task->session)) {
			InterlockedExchange((volatile LONG*)&task->curpos, task->curpos + task->len);
            return NULL;
        }
        tbuf = *task->buf;
        while(task->curpos < task->len) {
            size_t toread = min((size_t)task->len - (size_t)task->curpos, (size_t)bufsize);
            task->iomgr->preada_now(task->session, tbuf + task->curpos, toread, task->curpos);
            InterlockedExchange((volatile LONG*) &task->curpos, task->curpos + toread);
        }
            
        return NULL;
    }
    
    
    static size_t get_filesize(std::string filename) {
        std::string fname = filename;
        FILE * f = fopen(fname.c_str(), "r");
        
        if (f == NULL) {
            logstream(LOG_ERROR) << "Could not open file " << filename << " error: " << strerror(errno) << std::endl;
            assert(false);
        }
        
        fseek(f, 0, SEEK_END);
        size_t sz = ftell(f);
        fclose(f);
        return sz;
    }
    
    
    
}


#endif


