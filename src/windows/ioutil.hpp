
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
 * I/O Utils.
 */
#ifndef DEF_IOUTIL_HPP
#define DEF_IOUTIL_HPP

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
 

// Reads given number of bytes to a buffer
template <typename T>
void preada(FILE * f, T * tbuf, size_t nbytes, size_t off) {
    size_t nread = 0;
    char * buf = (char*)tbuf;
    while(nread<nbytes) {
        fseek(f, off + nread, SEEK_SET);
        size_t a = fread(buf, 1, nbytes - nread, f);
        if (a == (-1)) {
            std::cout << "Error, could not read: " << strerror(errno) << "; file-desc: " << f << std::endl;
            std::cout << "Pread arguments: " << f << " tbuf: " << tbuf << " nbytes: " << nbytes << " off: " << off << std::endl;
            assert(a != (-1));
        }
        assert(a>0);
        buf += a;
        nread += a;
    }
    assert(nread <= nbytes);
}

template <typename T>
void preada_trunc(FILE * f, T * tbuf, size_t nbytes, size_t off) {
    size_t nread = 0;
    char * buf = (char*)tbuf;
    while(nread<nbytes) {
        fseek(f, off + nread, SEEK_SET);
        size_t a = fread(buf, 1, nbytes - nread, f);
        if (a == 0) {
            // set rest to 0
     //       std::cout << "WARNING: file was not long enough - filled with zeros. " << std::endl;
            memset(buf, 0, nbytes-nread);
            return;
        }
        buf += a;
        nread += a;
    }

} 

template <typename T>
size_t readfull(FILE * f, T ** buf) {
     fseek(f, 0, SEEK_END);
     size_t sz = ftell(f);
     fseek(f, 0, SEEK_SET);
     *buf = (char*)malloc(sz);
    preada(f, *buf, sz, 0);
    return sz;
}
 template <typename T>
void pwritea(FILE * f, T * tbuf, size_t nbytes, size_t off) {
    size_t nwritten = 0;
    assert(f>0);
    char * buf = (char*)tbuf;
    while(nwritten<nbytes) {
        int err = fseek(f, off + nwritten, SEEK_SET);
        assert(err == 0);
        size_t a = fwrite(buf, 1, nbytes-nwritten, f);
        if (a == size_t(-1)) {
            logstream(LOG_ERROR) << "f:" << f << " nbytes: " << nbytes << " written: " << nwritten << " off:" << 
                off << " f: " << f << " error:" <<  strerror(errno) << std::endl;
            assert(false);
        }
        assert(a>0);
        buf += a;
        nwritten += a;
    }
} 
template <typename T>
void writea(FILE * f, T * tbuf, size_t nbytes) {
    size_t nwritten = 0;
    char * buf = (char*)tbuf;
    while(nwritten<nbytes) {
        size_t a = fwrite(buf, 1, nbytes-nwritten, f);
        assert(a>0);
        if (a == size_t(-1)) {
            logstream(LOG_ERROR) << "Could not write " << (nbytes-nwritten) << " bytes!" << " error:" <<  strerror(errno) << std::endl; 
            assert(false);
        }
        buf += a;
        nwritten += a;
    }
    fflush(f);

} 

template <typename T>
void checkarray_filesize(std::string fname, size_t nelements) {
    // Check the vertex file is correct size
    FILE * f = fopen(fname.c_str(),  "a");
    if (f == NULL) {
        logstream(LOG_ERROR) << "Error initializing the data-file: " << fname << " error:" <<  strerror(errno) << std::endl;    }
    assert(f != NULL);
    int err = ftruncate(fileno(f), nelements * sizeof(T));
    if (err != 0) {
        logstream(LOG_ERROR) << "Error in adjusting file size: " << fname << " to size: " << nelements * sizeof(T)    
                 << " error:" <<  strerror(errno) << std::endl;
    }
    assert(err == 0);
    fclose(f);
}

#endif


