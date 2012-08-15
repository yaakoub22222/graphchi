#ifndef SYNCHRONIZED_QUEUE_HPP
#define SYNCHRONIZED_QUEUE_HPP


#include <queue>
#include <deque>
#include "pthread_tools.hpp"

using namespace std;
// From graphlab

namespace graphchi {
    
        
        template <class T>
        class synchronized_queue {
            
        public:
            synchronized_queue() { };
            ~synchronized_queue() { };
            
            void push(const T &item) {
                _queuelock.lock();
                _queue.push(item);
                _queuelock.unlock();
            }
            
            bool safepop(T * ret) {
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
            
            T pop() {
                _queuelock.lock();
                T t = _queue.front();
                _queue.pop();
                _queuelock.unlock();
                return t;
            }
            
            size_t size() const{
                return _queue.size();
            }
        private:
			typedef std::deque<T, std::allocator<T> > TDEQUE;
			typedef std::queue<T,TDEQUE> TQUEUE;


            TQUEUE _queue;
            spinlock _queuelock;
        };
        
    }
#endif


