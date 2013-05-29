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
 * Sharder converts a graph into shards which the GraphChi engine
 * can process.
 */

/**
 * @section TODO
 * Change all C-style IO to Unix-style IO.
 */


#ifndef GRAPHCHI_SHARDER_DEF
#define GRAPHCHI_SHARDER_DEF


#include <iostream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <vector>
#include <omp.h>
#include <errno.h>
#include <sstream>
#include <string>

#include "api/chifilenames.hpp"
#include "api/graphchi_context.hpp"
#include "graphchi_types.hpp"
#include "io/stripedio.hpp"
#include "logger/logger.hpp"
#include "engine/auxdata/degree_data.hpp"
#include "metrics/metrics.hpp"
#include "metrics/reps/basic_reporter.hpp"
#include "preprocessing/formats/binary_adjacency_list.hpp"
#include "shards/memoryshard.hpp"
#include "shards/slidingshard.hpp"
#include "output/output.hpp"
#include "util/ioutil.hpp"
#include "util/qsort.hpp"
#include "util/kwaymerge.hpp"

namespace graphchi {
    template <typename VT, typename ET> class sharded_graph_output;
    
    
#define SHARDER_BUFSIZE (64 * 1024 * 1024)
    
    enum ProcPhase  { COMPUTE_INTERVALS=1, SHOVEL=2 };
    
    template <typename EdgeDataType>
    class DuplicateEdgeFilter {
    public:
        virtual bool acceptFirst(EdgeDataType& first, EdgeDataType& second) = 0;
    };
    
    
    template <typename EdgeDataType>
    struct edge_with_value {
        vid_t src;
        vid_t dst;
        EdgeDataType value;
        
#ifdef DYNAMICEDATA
        // For dynamic edge data, we need to know if the value needs to be added
        // to the vector, or are we storing an empty vector.
        bool is_chivec_value;
        uint16_t valindex;
#endif
        edge_with_value() {}
        
        edge_with_value(vid_t src, vid_t dst, EdgeDataType value) : src(src), dst(dst), value(value) {
#ifdef DYNAMICEDATA
            is_chivec_value = false;
            valindex = 0;
#endif
        }
        
        bool stopper() { return src == 0 && dst == 0; }
    };
    
    template <typename EdgeDataType>
    bool edge_t_src_less(const edge_with_value<EdgeDataType> &a, const edge_with_value<EdgeDataType> &b) {
        if (a.src == b.src) {
#ifdef DYNAMICEDATA
            if (a.dst == b.dst) {
                return a.valindex < b.valindex;
            }
#endif
            return a.dst < b.dst;
        }
        return a.src < b.src;
    }
    
    template <typename EdgeDataType>
    bool edge_t_dst_less(const edge_with_value<EdgeDataType> &a, const edge_with_value<EdgeDataType> &b) {
        if (a.dst == b.dst) {
#ifdef DYNAMICEDATA
            if (a.src == b.src) {
                return a.valindex < b.valindex;
            }
#endif
            return a.src < b.src;
        }
        return a.dst < b.dst;
    }
    
    
    template <typename EdgeDataType>
    struct shovel_merge_source : public merge_source<edge_with_value<EdgeDataType> > {
        
        size_t bufsize_bytes;
        size_t bufsize_edges;
        std::string shovelfile;
        int idx;
        int bufoff;
        edge_with_value<EdgeDataType> * buffer;
        int f;
        size_t numedges;
        
        shovel_merge_source(size_t bufsize_bytes, std::string shovelfile) : bufsize_bytes(bufsize_bytes), 
        shovelfile(shovelfile), idx(0), bufoff(0) {
            f = open(shovelfile.c_str(), O_RDONLY);
            
            buffer = (edge_with_value<EdgeDataType> *) malloc(bufsize_bytes);
            numedges = get_filesize(shovelfile) / sizeof(edge_with_value<EdgeDataType> );
            bufsize_edges = bufsize_bytes / sizeof(edge_with_value<EdgeDataType>);
            load_next();
        }
        
        ~shovel_merge_source() {
            close(f);
            free(buffer);
        }
        
        void load_next() {
            size_t len = std::max(bufsize, (numedges - idx) * edge_with_value<EdgeDataType>);
            preada(f, buffer, len, idx * sizeof(edge_with_value<EdgeDataType>));
        }
        
        bool has_more() {
            return idx < numedges;
        }
        
        edge_with_value<EdgeDataType> next() {
            assert(idx < numedges);
            if (bufidx == bufsize_edges) {
                load_next();
            }
            idx++;
            return buffer[bufidx++];
        }
    };
    
    template <typename EdgeDataType>
    class sharder : public merge_sink<edge_with_value<EdgeDataType> > {
        
        typedef edge_with_value<EdgeDataType> edge_t;
        
    protected:
        std::string basefilename;
        
        vid_t max_vertex_id;
        
        /* Sharding */
        int nshards;
        std::vector< std::pair<vid_t, vid_t> > intervals;
        
        int phase;
        
        int * edgecounts;
        int vertexchunk;
        size_t nedges;
        std::string prefix;
        
        int compressed_block_size;
        
        edge_t ** bufs;
        int * bufptrs;
        size_t bufsize;
        size_t edgedatasize;
        size_t ebuffer_size;
        size_t edges_per_block;
        
        vid_t filter_max_vertex;
        
        DuplicateEdgeFilter<EdgeDataType> * duplicate_edge_filter;
        
        bool no_edgevalues;
#ifdef DYNAMICEDATA
        edge_t last_added_edge;
#endif
        
        metrics m;
        
        binary_adjacency_list_writer<EdgeDataType> * preproc_writer;
        
        size_t curshovel_idx;
        size_t shovelsize;
        int numshovels;
        size_t shoveled_edges;
        edge_with_value<EdgeDataType> * curshovel_buffer;
        
    public:
        
        sharder(std::string basefilename) : basefilename(basefilename), m("sharder"), preproc_writer(NULL) {            bufs = NULL;
            edgedatasize = sizeof(EdgeDataType);
            no_edgevalues = false;
            compressed_block_size = 4096 * 1024;
            filter_max_vertex = 0;
            while (compressed_block_size % sizeof(EdgeDataType) != 0) compressed_block_size++;
            edges_per_block = compressed_block_size / sizeof(EdgeDataType);
            duplicate_edge_filter = NULL;
        }
        
        
        virtual ~sharder() {
            delete curshovel_buffer;
        }
        
        void set_duplicate_filter(DuplicateEdgeFilter<EdgeDataType> * filter) {
            this->duplicate_edge_filter = filter;
        }
        
        void set_max_vertex_id(vid_t maxid) {
            filter_max_vertex = maxid;
        }
        
        void set_no_edgevalues() {
            no_edgevalues = true;
        }
        
        /**
         * Call to start a preprocessing session.
         */
        void start_preprocessing() {
            numshovels = 0;
            shovelsize = (1024 * 1024 * get_option_int("membudget_mb", 1024) / 4 / sizeof(edge_with_value<EdgeDataType>));
            curshovel_idx = 0;
            curshovel_buffer = (edge_with_value<EdgeDataType> *) calloc(shovelsize, sizeof(edge_with_value<EdgeDataType>));
            
            /* Write the maximum vertex id place holder - to be filled later */
            max_vertex_id = 0;
            shoveled_edges = 0;
        }
        
        /**
         * Call to finish the preprocessing session.
         */
        void end_preprocessing() {
            flush_shovel();
        }
        
        void flush_shovel() {
            std::string fname = shovel_filename(curshovel_idx);
            int f = open(fname.c_str(), O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            
            /* Sort */
            // TODO: remove duplicates here!
            m.start_time("shovel_sort");
            logstream(LOG_INFO) << "Sorting shovel: " << numshovels << std::endl;
            quickSort(curshovel_buffer, curshovel_idx, edge_t_dst_less<EdgeDataType>);
            m.stop_time("shovel_short");
            
            m.start_time("shovel_write");
            writea(f, curshovel_buffer, curshovel_idx * sizeof(edge_with_value<EdgeDataType>));
            close(f);
            m.stop_time("shovel_write");
            numshovels++;
        }
        
        /**
         * Add edge to be preprocessed with a value.
         */
        void preprocessing_add_edge(vid_t from, vid_t to, EdgeDataType val) {
            if (from == to) {
                // Do not allow self-edges
                return;
            }
            curshovel_buffer[curshovel_idx++] = edge_with_value<EdgeDataType>(from, to, val);
            
            if (curshovel_idx == shovelsize) flush_shovel();
            
            max_vertex_id = std::max(std::max(from, to), max_vertex_id);
            shoveled_edges++;
        }
        
#ifdef DYNAMICEDATA
        void preprocessing_add_edge_multival(vid_t from, vid_t to, std::vector<EdgeDataType> & vals) {
            typename std::vector<EdgeDataType>::iterator iter;
            for(iter=vals.begin(); iter != vals.end(); ++iter) {
                preprocessing_add_edge(from, to, *iter);
            }
            max_vertex_id = std::max(std::max(from, to), max_vertex_id);
        }
        
#endif
        
        /**
         * Add edge without value to be preprocessed
         */
        void preprocessing_add_edge(vid_t from, vid_t to) {
            preproc_writer->add_edge(from, to);
            max_vertex_id = std::max(std::max(from, to), max_vertex_id);
        }
        
        /** Buffered write function */
        template <typename T>
        void bwrite(int f, char * buf, char * &bufptr, T val) {
            if (bufptr + sizeof(T) - buf >= SHARDER_BUFSIZE) {
                writea(f, buf, bufptr - buf);
                bufptr = buf;
            }
            *((T*)bufptr) = val;
            bufptr += sizeof(T);
        }
        
        int blockid;
        
        template <typename T>
        void edata_flush(char * buf, char * bufptr, std::string & shard_filename, size_t totbytes) {
            int len = (int) (bufptr - buf);
            
            m.start_time("edata_flush");
            
            std::string block_filename = filename_shard_edata_block(shard_filename, blockid, compressed_block_size);
            int f = open(block_filename.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            write_compressed(f, buf, len);
            close(f);
            
            m.stop_time("edata_flush");
            
            
#ifdef DYNAMICEDATA
            // Write block's uncompressed size
            write_block_uncompressed_size(block_filename, len);
            
#endif
            
            blockid++;
        }
        
        template <typename T>
        void bwrite_edata(char * &buf, char * &bufptr, T val, size_t & totbytes, std::string & shard_filename,
                          size_t & edgecounter) {
            if (no_edgevalues) return;
            
            if (edgecounter == edges_per_block) {
                edata_flush<T>(buf, bufptr, shard_filename, totbytes);
                bufptr = buf;
                edgecounter = 0;
            }
            
            // Check if buffer is big enough
            if (bufptr - buf + sizeof(T) > ebuffer_size) {
                ebuffer_size *= 2;
                logstream(LOG_DEBUG) << "Increased buffer size to: " << ebuffer_size << std::endl;
                size_t ptroff = bufptr - buf; // Remember the offset
                buf = (char *) realloc(buf, ebuffer_size);
                bufptr = buf + ptroff;
            }
            
            totbytes += sizeof(T);
            *((T*)bufptr) = val;
            bufptr += sizeof(T);
        }
        
        
        /**
         * Executes sharding.
         * @param nshards_string the number of shards as a number, or "auto" for automatic determination
         */
        int execute_sharding(std::string nshards_string) {
            m.start_time("execute_sharding");
            
            nshards = determine_number_of_shards();
            write_shards();
            
            m.stop_time("execute_sharding");
            
            /* Print metrics */
            basic_reporter basicrep;
            m.report(basicrep);
            
            return nshards;
        }
        
        /**
         * Sharding. This code might be hard to read - modify very carefully!
         */
    protected:
        
        virtual void determine_number_of_shards(std::string nshards_string) {
            assert(preprocessed_file_exists());
            if (nshards_string.find("auto") != std::string::npos || nshards_string == "0") {
                logstream(LOG_INFO) << "Determining number of shards automatically." << std::endl;
                
                int membudget_mb = get_option_int("membudget_mb", 1024);
                logstream(LOG_INFO) << "Assuming available memory is " << membudget_mb << " megabytes. " << std::endl;
                logstream(LOG_INFO) << " (This can be defined with configuration parameter 'membudget_mb')" << std::endl;
                
                size_t numedges = shoveled_edges; 
                
                double max_shardsize = membudget_mb * 1024. * 1024. / 8;
                logstream(LOG_INFO) << "Determining maximum shard size: " << (max_shardsize / 1024. / 1024.) << " MB." << std::endl;
                
                nshards = (int) ( 2 + (numedges * sizeof(EdgeDataType) / max_shardsize) + 0.5);
                
#ifdef DYNAMICEDATA
                // For dynamic edge data, more working memory is needed, thus the number of shards is larger.
                nshards = (int) ( 2 + 4 * (numedges * sizeof(EdgeDataType) / max_shardsize) + 0.5);
#endif
                
            } else {
                nshards = atoi(nshards_string.c_str());
            }
            assert(nshards > 0);
            logstream(LOG_INFO) << "Number of shards to be created: " << nshards << std::endl;
        }
        
        
    protected:
        
        void one_shard_intervals() {
            assert(nshards == 1);
            std::string fname = filename_intervals(basefilename, nshards);
            FILE * f = fopen(fname.c_str(), "w");
            intervals.push_back(std::pair<vid_t,vid_t>(0, max_vertex_id));
            fprintf(f, "%u\n", max_vertex_id);
            fclose(f);
            
            /* Write meta-file with the number of vertices */
            std::string numv_filename = basefilename + ".numvertices";
            f = fopen(numv_filename.c_str(), "w");
            fprintf(f, "%u\n", 1 + max_vertex_id);
            fclose(f);
            
            assert(nshards == (int)intervals.size());
        }
        
        
        std::string shovel_filename(int idx) {
            std::stringstream ss;
            ss << basefilename << idx << "." << nshards << ".shovel";
            return ss.str();
        }
        
        
        int lastpart;
        
        
        
        
        degree * degrees;
        
        virtual void finish_shard(int shard, edge_t * shovelbuf, size_t shovelsize) {
            m.start_time("shard_final");
            blockid = 0;
            size_t edgecounter = 0;
            
            logstream(LOG_INFO) << "Starting final processing for shard: " << shard << std::endl;
            
            std::string fname = filename_shard_adj(basefilename, shard, nshards);
            std::string edfname = filename_shard_edata<EdgeDataType>(basefilename, shard, nshards);
            std::string edblockdirname = dirname_shard_edata_block(edfname, compressed_block_size);
            
            /* Make the block directory */
            if (!no_edgevalues)
                mkdir(edblockdirname.c_str(), 0777);
            size_t numedges = shovelsize / sizeof(edge_t);
            
            logstream(LOG_DEBUG) << "Shovel size:" << shovelsize << " edges: " << numedges << std::endl;
            
            quickSort(shovelbuf, (int)numedges, edge_t_src_less<EdgeDataType>);
            
            // Remove duplicates
            if (duplicate_edge_filter != NULL && numedges > 0) {
                edge_t * tmpbuf = (edge_t*) calloc(numedges, sizeof(edge_t));
                size_t i = 1;
                tmpbuf[0] = shovelbuf[0];
                for(size_t j=1; j<numedges; j++) {
                    edge_t prev = tmpbuf[i - 1];
                    edge_t cur = shovelbuf[j];
                    
                    if (prev.src == cur.src && prev.dst == cur.dst) {
                        if (duplicate_edge_filter->acceptFirst(cur.value, prev.value)) {
                            // Replace the edge with the newer one
                            tmpbuf[i - 1] = cur;
                        }
                    } else {
                        tmpbuf[i++] = cur;
                    }
                }
                numedges = i;
                logstream(LOG_DEBUG) << "After duplicate elimination: " << numedges << " edges" << std::endl;
                free(shovelbuf);
                shovelbuf = tmpbuf; tmpbuf = NULL;
            }
            
            // Create the final file
            int f = open(fname.c_str(), O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            if (f < 0) {
                logstream(LOG_ERROR) << "Could not open " << fname << " error: " << strerror(errno) << std::endl;
            }
            assert(f >= 0);
            int trerr = ftruncate(f, 0);
            assert(trerr == 0);
            
            char * buf = (char*) malloc(SHARDER_BUFSIZE);
            char * bufptr = buf;
            
            char * ebuf = (char*) malloc(compressed_block_size);
            ebuffer_size = compressed_block_size;
            char * ebufptr = ebuf;
            
            vid_t curvid=0;
#ifdef DYNAMICEDATA
            vid_t lastdst = 0xffffffff;
            int jumpover = 0;
            size_t num_uniq_edges = 0;
            size_t last_edge_count = 0;
#endif
            size_t istart = 0;
            size_t tot_edatabytes = 0;
            for(size_t i=0; i <= numedges; i++) {
#ifdef DYNAMICEDATA
                i += jumpover;  // With dynamic values, there might be several values for one edge, and thus the edge repeated in the data.
                jumpover = 0;
#endif //DYNAMICEDATA
                edge_t edge = (i < numedges ? shovelbuf[i] : edge_t(0, 0, EdgeDataType())); // Last "element" is a stopper
                
#ifdef DYNAMICEDATA
                
                if (lastdst == edge.dst && edge.src == curvid) {
                    // Currently not supported
                    logstream(LOG_ERROR) << "Duplicate edge in the stream - aborting" << std::endl;
                    assert(false);
                }
                lastdst = edge.dst;
#endif
                
                if (!edge.stopper()) {
#ifndef DYNAMICEDATA
                    bwrite_edata<EdgeDataType>(ebuf, ebufptr, EdgeDataType(edge.value), tot_edatabytes, edfname, edgecounter);
#else
                    /* If we have dynamic edge data, we need to write the header of chivector - if there are edge values */
                    if (edge.is_chivec_value) {
                        // Need to check how many values for this edge
                        int count = 1;
                        while(shovelbuf[i + count].valindex == count) { count++; }
                        
                        assert(count < 32768);
                        
                        typename chivector<EdgeDataType>::sizeword_t szw;
                        ((uint16_t *) &szw)[0] = (uint16_t)count;  // Sizeword with length and capacity = count
                        ((uint16_t *) &szw)[1] = (uint16_t)count;
                        bwrite_edata<typename chivector<EdgeDataType>::sizeword_t>(ebuf, ebufptr, szw, tot_edatabytes, edfname, edgecounter);
                        for(int j=0; j < count; j++) {
                            bwrite_edata<EdgeDataType>(ebuf, ebufptr, EdgeDataType(shovelbuf[i + j].value), tot_edatabytes, edfname, edgecounter);
                        }
                        jumpover = count - 1; // Jump over
                    } else {
                        // Just write size word with zero
                        bwrite_edata<int>(ebuf, ebufptr, 0, tot_edatabytes, edfname, edgecounter);
                    }
                    num_uniq_edges++;
                    
#endif
                    edgecounter++; // Increment edge counter here --- notice that dynamic edata case makes two or more calls to bwrite_edata before incrementing
                }
                if (degrees != NULL && edge.src != edge.dst) {
                    degrees[edge.src].outdegree++;
                    degrees[edge.dst].indegree++;
                }
                
                if ((edge.src != curvid) || edge.stopper()) {
                    // New vertex
#ifndef DYNAMICEDATA
                    size_t count = i - istart;
#else
                    size_t count = num_uniq_edges - 1 - last_edge_count;
                    last_edge_count = num_uniq_edges - 1;
                    if (edge.stopper()) count++;  
#endif
                    assert(count>0 || curvid==0);
                    if (count>0) {
                        if (count < 255) {
                            uint8_t x = (uint8_t)count;
                            bwrite<uint8_t>(f, buf, bufptr, x);
                        } else {
                            bwrite<uint8_t>(f, buf, bufptr, 0xff);
                            bwrite<uint32_t>(f, buf, bufptr, (uint32_t)count);
                        }
                    }
                    
#ifndef DYNAMICEDATA
                    for(size_t j=istart; j < i; j++) {
                        bwrite(f, buf, bufptr,  shovelbuf[j].dst);
                    }
#else
                    // Special dealing with dynamic edata because some edges can be present multiple
                    // times in the shovel.
                    for(size_t j=istart; j < i; j++) {
                        if (j == istart || shovelbuf[j - 1].dst != shovelbuf[j].dst) {
                            bwrite(f, buf, bufptr,  shovelbuf[j].dst);
                        }
                    }
#endif
                    istart = i;
#ifdef DYNAMICEDATA
                    istart += jumpover;
#endif
                    
                    // Handle zeros
                    if (!edge.stopper()) {
                        if (edge.src - curvid > 1 || (i == 0 && edge.src>0)) {
                            int nz = edge.src - curvid - 1;
                            if (i == 0 && edge.src > 0) nz = edge.src; // border case with the first one
                            do {
                                bwrite<uint8_t>(f, buf, bufptr, 0);
                                nz--;
                                int tnz = std::min(254, nz);
                                bwrite<uint8_t>(f, buf, bufptr, (uint8_t) tnz);
                                nz -= tnz;
                            } while (nz>0);
                        }
                    }
                    curvid = edge.src;
                }
            }
            
            /* Flush buffers and free memory */
            writea(f, buf, bufptr - buf);
            free(buf);
            free(shovelbuf);
            close(f);
            
            /* Write edata size file */
            if (!no_edgevalues) {
                edata_flush<EdgeDataType>(ebuf, ebufptr, edfname, tot_edatabytes);
                
                std::string sizefilename = edfname + ".size";
                std::ofstream ofs(sizefilename.c_str());
#ifndef DYNAMICEDATA
                ofs << tot_edatabytes;
#else
                ofs << num_uniq_edges * sizeof(int); // For dynamic edge data, write the number of edges.
#endif
                
                ofs.close();
            }
            free(ebuf);
            
            m.stop_time("shard_final");
        }
        
        /* Begin: Kway -merge sink interface */
        
        size_t edges_per_shard;
        size_t cur_shard_counter;
        size_t shard_capacity;
        int shardnum;
        edge_with_value<EdgeDataType> * sinkbuffer;
        vid_t prevvid;
        vid_t this_interval_start;
        
        virtual void add(edge_with_value<EdgeDataType> val) {
            if (cur_shard_counter >= edges_per_shard && val.src != prevvid) {
                createnextshard();
            }
            
            if (cur_shard_counter == shard_capacity) {
                /* Really should have a way to limit shard sizes, but probably not needed in practice */
                logstream(LOG_WARNING) << "Shard " << shardnum << " overflowing! " << cur_shard_counter << " / " << shard_capacity << std::endl;
                shard_capacity = (size_t) 1.2 * shard_capacity;
                sinkbuffer = realloc(sinkbuffer, shard_capacity * sizeof(edge_with_value<EdgeDataType>));
            }
            
            sinkbuffer[cur_shard_counter++] = val;
            prevvid = val.src;
        }
        
        void createnextshard() {
            assert(shardnum < nshards);
            intervals.push_back(std::pair<vid_t, vid_t>(this_interval_start, prevvid));
            this_interval_start = prevvid + 1;
            finish_shard(shardnum++, sinkbuffer, cur_shard_counter);
            cur_shard_counter = 0;
        }
        
        virtual void done() {
            createnextshard();
            assert(shardnum == nshards);
            free(sinkbuffer);
            sinkbuffer = NULL;
            
            /* Write intervals */
            std::string fname = filename_intervals(basefilename, nshards);
            FILE * f = fopen(fname.c_str(), "w");
            
            if (f == NULL) {
                logstream(LOG_ERROR) << "Could not open file: " << fname << " error: " <<
                strerror(errno) << std::endl;
            }
            assert(f != NULL);
            for(int i=0; i<intervals.size(); i++) {
               fprintf(f, "%u\n", intervals[i].second);
            }
            fclose(f);
            
            /* Write meta-file with the number of vertices */
            std::string numv_filename = basefilename + ".numvertices";
            f = fopen(numv_filename.c_str(), "w");
            fprintf(f, "%u\n", 1 + max_vertex_id);
            fclose(f);
        }
        
        /* End: Kway -merge sink interface */
        
        
        
        /**
         * Write the shard by sorting the shovel file and compressing the
         * adjacency information.
         * To support different shard types, override this function!
         */
        virtual void write_shards() {
            
            size_t membudget_mb = (size_t) get_option_int("membudget_mb", 1024);
            
            // Check if we have enough memory to keep track
            // of the vertex degrees in-memory (heuristic)
            bool count_degrees_inmem = membudget_mb * 1024 * 1024 / 3 > max_vertex_id * sizeof(degree);
            degrees = NULL;
#ifdef DYNAMICEDATA
            if (!count_degrees_inmem) {
                /* Temporary: force in-memory count of degrees because the PSW-based computation
                 is not yet compatible with dynamic edge data.
                 */
                logstream(LOG_WARNING) << "Dynamic edge data support only sharding when the vertex degrees can be computed in-memory." << std::endl;
                logstream(LOG_WARNING) << "If the program gets very slow (starts swapping), the data size is too big." << std::endl;
                count_degrees_inmem = true;
            }
#endif
            if (count_degrees_inmem) {
                degrees = (degree *) calloc(1 + max_vertex_id, sizeof(degree));
            }
            
            // KWAY MERGE
            edges_per_shard = shoveled_edges / nshards + 1;
            shard_capacity = edges_per_shard / 3 * 2;  // Shard can go 50% over
            shardnum = 0;
            this_interval_start = 0;
            sinkbuffer = (edge_with_value<EdgeDataType> *) calloc(edges_per_shard, sizeof(edge_with_value<EdgeDataType>));
            logstream(LOG_DEBUG) << "Edges per shard: " << edges_per_shard << std::endl;
            cur_shard_counter = 0;
            
            /* Initialize kway merge sources */
            size_t B = membudget_mb / 2 / numshovels;
            prevvid = (-1);
            std::vector< merge_source<edge_with_value<EdgeDataType> > *> sources;
            for(int i=0; i < numshovels; i++) {
                sources.push_back(new shovel_merge_source(B, shovel_filename(i)));
            }
            
            kway_merge merger(sources, this);
            merger.merge();
            
            // Delete sources
            for(int i=0; i < numshovels; i++) {
                delete(sources[i]);
            }
            
            
            if (!count_degrees_inmem) {
#ifndef DYNAMICEDATA
                // Use memory-efficient (but slower) method to create degree-data
                create_degree_file();
#endif
                
            } else {
                std::string degreefname = filename_degree_data(basefilename);
                int degreeOutF = open(degreefname.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
                if (degreeOutF < 0) {
                    logstream(LOG_ERROR) << "Could not create: " << degreeOutF << std::endl;
                    assert(degreeOutF >= 0);
                }
                
                writea(degreeOutF, degrees, sizeof(degree) * (1 + max_vertex_id));
                free(degrees);
                close(degreeOutF);
            }
            
        }
        
        
        typedef char dummy_t;
        
        typedef sliding_shard<int, dummy_t> slidingshard_t;
        typedef memory_shard<int, dummy_t> memshard_t;
        
        
#ifndef DYNAMICEDATA
        void create_degree_file() {
            // Initialize IO
            stripedio * iomgr = new stripedio(m);
            std::vector<slidingshard_t * > sliding_shards;
            
            int subwindow = 5000000;
            m.set("subwindow", (size_t)subwindow);
            
            int loadthreads = 4;
            
            m.start_time("degrees.runtime");
            
            /* Initialize streaming shards */
            int blocksize = compressed_block_size;
            
            for(int p=0; p < nshards; p++) {
                logstream(LOG_INFO) << "Initialize streaming shard: " << p << std::endl;
                sliding_shards.push_back(
                                         new slidingshard_t(iomgr, filename_shard_edata<dummy_t>(basefilename, p, nshards),
                                                            filename_shard_adj(basefilename, p, nshards), intervals[p].first,
                                                            intervals[p].second,
                                                            blocksize, m, true, true));
            }
            
            graphchi_context ginfo;
            ginfo.nvertices = 1 + intervals[nshards - 1].second;
            ginfo.scheduler = NULL;
            
            std::string outputfname = filename_degree_data(basefilename);
            
            int degreeOutF = open(outputfname.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            if (degreeOutF < 0) {
                logstream(LOG_ERROR) << "Could not create: " << degreeOutF << std::endl;
            }
            assert(degreeOutF >= 0);
            int trerr = ftruncate(degreeOutF, ginfo.nvertices * sizeof(int) * 2);
            assert(trerr == 0);
            for(int window=0; window<nshards; window++) {
                metrics_entry mwi = m.start_time();
                
                vid_t interval_st = intervals[window].first;
                vid_t interval_en = intervals[window].second;
                
                /* Flush stream shard for the window */
                sliding_shards[window]->flush();
                
                /* Load shard[window] into memory */
                memshard_t memshard(iomgr, filename_shard_edata<EdgeDataType>(basefilename, window, nshards), filename_shard_adj(basefilename, window, nshards),
                                    interval_st, interval_en, blocksize, m);
                memshard.only_adjacency = true;
                logstream(LOG_INFO) << "Interval: " << interval_st << " " << interval_en << std::endl;
                
                for(vid_t subinterval_st=interval_st; subinterval_st <= interval_en; ) {
                    vid_t subinterval_en = std::min(interval_en, subinterval_st + subwindow);
                    logstream(LOG_INFO) << "(Degree proc.) Sub-window: [" << subinterval_st << " - " << subinterval_en << "]" << std::endl;
                    assert(subinterval_en >= subinterval_st && subinterval_en <= interval_en);
                    
                    /* Preallocate vertices */
                    metrics_entry men = m.start_time();
                    int nvertices = subinterval_en - subinterval_st + 1;
                    std::vector< graphchi_vertex<int, dummy_t> > vertices(nvertices, graphchi_vertex<int, dummy_t>()); // preallocate
                    
                    
                    for(int i=0; i < nvertices; i++) {
                        vertices[i] = graphchi_vertex<int, dummy_t>(subinterval_st + i, NULL, NULL, 0, 0);
                        vertices[i].scheduled =  true;
                    }
                    
                    metrics_entry me = m.start_time();
                    omp_set_num_threads(loadthreads);
#pragma omp parallel for
                    for(int p=-1; p < nshards; p++)  {
                        if (p == (-1)) {
                            // if first window, now need to load the memshard
                            if (memshard.loaded() == false) {
                                memshard.load();
                            }
                            
                            /* Load vertices from memshard (only inedges for now so can be done in parallel) */
                            memshard.load_vertices(subinterval_st, subinterval_en, vertices);
                        } else {
                            /* Stream forward other than the window partition */
                            if (p != window) {
                                sliding_shards[p]->read_next_vertices(nvertices, subinterval_st, vertices, false);
                            }
                        }
                    }
                    
                    m.stop_time(me, "stream_ahead", window);
                    
                    
                    metrics_entry mev = m.start_time();
                    // Read first current values
                    
                    int * vbuf = (int*) malloc(nvertices * sizeof(int) * 2);
                    
                    for(int i=0; i<nvertices; i++) {
                        vbuf[2 * i] = vertices[i].num_inedges();
                        vbuf[2 * i +1] = vertices[i].num_outedges();
                    }
                    pwritea(degreeOutF, vbuf, nvertices * sizeof(int) * 2, subinterval_st * sizeof(int) * 2);
                    
                    free(vbuf);
                    
                    // Move window
                    subinterval_st = subinterval_en+1;
                }
                /* Move the offset of the window-shard forward */
                sliding_shards[window]->set_offset(memshard.offset_for_stream_cont(), memshard.offset_vid_for_stream_cont(),
                                                   memshard.edata_ptr_for_stream_cont());
            }
            close(degreeOutF);
            m.stop_time("degrees.runtime");
            delete iomgr;
        }
#endif
        
        friend class binary_adjacency_list_reader<EdgeDataType>;
        template <typename A, typename B> friend class sharded_graph_output;
    }; // End class sharder
    
    
    /**
     * Outputs new edges into a shard - can be used from an update function
     */
    template <typename VT, typename ET>
    class sharded_graph_output : public ioutput<VT, ET> {
        
        sharder<ET> * sharderobj;
        mutex lock;
        
    public:
        sharded_graph_output(std::string filename, std::vector< std::pair<vid_t, vid_t> > intervals, DuplicateEdgeFilter<ET> * filter = NULL) {
            sharderobj = new sharder<ET>(filename);
            sharderobj->set_duplicate_filter(filter);
            sharderobj->start_preprocessing();
        }
        
        ~sharded_graph_output() {
            delete sharderobj;
            sharderobj = NULL;
        }
        
        
        
    public:
        void output_edge(vid_t from, vid_t to) {
            assert(false); // Need to use the custom method
        }
        
        
        
        
        virtual void output_edge(vid_t from, vid_t to, float value) {
            assert(false); // Need to use the custom method
        }
        
        virtual void output_edge(vid_t from, vid_t to, double value) {
            assert(false); // Need to use the custom method
        }
        
        
        virtual void output_edge(vid_t from, vid_t to, int value)  {
            assert(false); // Need to use the custom method
        }
        
        virtual void output_edge(vid_t from, vid_t to, size_t value)  {
            assert(false); // Need to use the custom method
        }
        
        void output_edgeval(vid_t from, vid_t to, ET value) {
            lock.lock();
            sharderobj->preprocessing_add_edge(from, to, value);
            assert(false);
            lock.unlock();
        }
        
        void output_value(vid_t vid, VT value) {
            assert(false);  // Not used here
        }
        
        
        void close() {
            //  sharderobj->end_phase();
        }
        
        size_t finish_sharding() {
            // sharderobj->write_shards();
            return sharderobj->nshards;
        }
        
    };
    
}; // namespace


#endif



