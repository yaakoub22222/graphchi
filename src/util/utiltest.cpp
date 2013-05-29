/*
 *  utiltest.cpp
 *  graphchi_xcode
 *
 *  Created by Aapo Kyrola on 5/29/13.
 *  Copyright 2013 Carnegie Mellon University. All rights reserved.
 *
 */

#include "utiltest.h"

#include "binary_minheap.hpp"
#include <iostream>

 
int main() {
    int N = 1000;
    binary_minheap<int> heap(N);
    
    for(int j=0; j<N; j++) {
        int x = random();
        if (x == 0) x = 1; 
        std::cout << "insert:" << x << std::endl;
        heap.insert(x);
    }
    
    int last = 0;
    for(int j=0; j<N; j++) {
        assert(!heap.empty());

        int x = heap.min();
        assert(x != 0);
        heap.extractMin();
        assert(j ==0 || x >= last);
        std::cout << x << std::endl;
        last = x;
    }
    
    assert(heap.empty());
}
