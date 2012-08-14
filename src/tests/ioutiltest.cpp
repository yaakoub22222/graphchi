//
//  ioutiltest.cpp
//  graphchi_xcode
//
//  Created by Aapo Kyrola on 8/14/12.
//
//
#include <assert.h>
#include <iostream>
#include <stdio.h>
#include <cmath>
#define WINDOWS 1

#include "graphchi_basic_includes.hpp"
#include "graphchi_types.hpp"
#include "util/ioutil.hpp"
#include "io/stripedio.hpp"



struct teststruct {
    int a;
    float b;
    bool c;
    teststruct() {}
    teststruct(int a, float b, bool c) : a(a), b(b), c(c) {}
};

using namespace graphchi;

int main(int argc, const char ** argv) {
    
    std::string fname = "./testfile";
    
    FILE * f = fopen(fname.c_str(), "w+b");

    for(int i=0; i < 1000; i++) {
        teststruct s(i, i * .5f, i % 2);
        writea(f, &s, sizeof(teststruct));
        
        preada(f, &s, sizeof(teststruct), i * sizeof(teststruct));
        assert(s.a == i);
        assert(fabsf(s.b - i * .5f) < 1e-5);
        assert(s.c == i % 2);
    }
    
    std::cout << get_filesize(fname) << std::endl;
    assert(get_filesize(fname) == 1000 * sizeof(teststruct));
    
    for(int i=0; i < 1000; i++) {
        teststruct s;
        preada(f, &s, sizeof(teststruct), i * sizeof(teststruct));
        assert(s.a == i);
        assert(s.b == i * .5f);
        assert(s.c == i % 2);
    }
    
    // random writes
    for(int i=0; i < 1000; i++) {
        int j = (i * 3333 - i * 77) % 1000;
        teststruct s(j, j * .3f, j % 9);
        pwritea(f, &s, sizeof(teststruct), sizeof(teststruct) * j);
    }
    
    std::cout << "Random tests..." << std::endl;
    
    for(int i=0; i < 1000; i++) {
        int j = (i * 3333 - i * 77) % 1000;
        teststruct ts(j, j * .3f, j % 9);
        teststruct s;
        preada(f, &s, sizeof(teststruct), sizeof(teststruct) * j);
        assert(ts.a == s.a && ts.b == s.b && ts.c == s.c);
    }
    
    std::cout << "Success." << std::endl;
    fclose(f);
}