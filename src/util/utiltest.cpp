/*
 *  utiltest.cpp
 *  graphchi_xcode
 *
 *  Created by Aapo Kyrola on 5/29/13.
 *  Copyright 2013 Carnegie Mellon University. All rights reserved.
 *
 */
#include <iostream>
#include <vector>

#include "utiltest.h"

#include "binary_minheap.hpp"
#include "kwaymerge.hpp"



struct vectorsource : public merge_source<int> {
    std::vector<int> v;
    int j;
    vectorsource(int n) {
        for(int i=0; i<n; i++) {
            v.push_back(i * 10000 + random() % 10000); 
        }
        j=0;
    }
        
    virtual bool has_more() {
        return j < v.size();
    }
    virtual int next() {
        return v[j++]; 
    }
};

struct vectorsink : public merge_sink<int> { 
    std::vector<int> v;
    virtual void add(int val) {
        if (v.size() > 0) {
            assert(val >= v[v.size()-1]);
        }
        v.push_back(val);
    }
    virtual void done() {
        assert(v.size() > 0);
    }
};
 
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
    
    // TEST KWAY MERGE
    int nsources = 100;
    std::vector<merge_source<int> *> sources;
    int total = 0;
    vectorsink sink;
    for(int j=0; j<nsources; j++) {
        int n = 1000 + random() % 2000;
        total += n;
        sources.push_back(new vectorsource(n));
    }
    
    kway_merge<int> merger(sources, &sink);
    merger.merge();
    assert(sink.v.size() == total);
}
