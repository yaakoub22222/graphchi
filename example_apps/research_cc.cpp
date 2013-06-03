
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
 * Application for computing the connected components of a graph.
 * The algorithm is simple: on first iteration each vertex sends its
 * id to neighboring vertices. On subsequent iterations, each vertex chooses
 * the smallest id of its neighbors and broadcasts its (new) label to
 * its neighbors. The algorithm terminates when no vertex changes label.
 *
 * @section REMARKS
 *
 * Research connected components code.
 * @author Aapo Kyrola
 */

#define OUTPUTLEVEL LOG_ERROR
#define GRAPHCHI_DISABLE_COMPRESSION

#include <cmath>
#include <string>

#include "graphchi_basic_includes.hpp"

using namespace graphchi;

#define MAX_VIDT 0xffffffff

struct bidirectional_component_weight {
    vid_t smaller_component;
    vid_t larger_component;
    bidirectional_component_weight() {
        smaller_component = larger_component = MAX_VIDT;
    }
    
    bidirectional_component_weight(double x) {
        smaller_component = larger_component = MAX_VIDT;
    }
    
    
    vid_t neighbor_label(vid_t myid, vid_t nbid) {
        vid_t label = (myid < nbid ? larger_component : smaller_component);
        if (label == MAX_VIDT) label = nbid;  // NOTE: important optimization (for random orders!)
        return label;
    }
    
    vid_t & my_label(vid_t myid, vid_t nbid) {
        if (myid < nbid) {
            return smaller_component;
        } else {
            return larger_component;
        }
    }
    
    bool labels_agree() {
        return smaller_component == larger_component;
    }
    
};

/**
 * Type definitions. Remember to create suitable graph shards using the
 * Sharder-program.
 */
typedef vid_t VertexDataType;       // vid_t is the vertex id type
typedef bidirectional_component_weight EdgeDataType;

size_t num_agree = 0;
size_t num_disagree = 0;
int contracted = 0;
double contracted_after1 = 0, contracted_after2 = 0;
int finish_iterations=0;

/**
 * GraphChi programs need to subclass GraphChiProgram<vertex-type, edge-type>
 * class. The main logic is usually in the update function.
 */
struct ResearchCC : public GraphChiProgram<VertexDataType, EdgeDataType> {
    
    /**
     */
    void update(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, graphchi_context &gcontext) {
        // Even iterations propagate, odd count agreements
        if (gcontext.iteration % 2 == 0) {
            /* Get my component id. It is the minimum label of a neighbor via a mst edge (or my own id) */
            vid_t min_component_id = vertex.id();
            for(int i=0; i < vertex.num_edges(); i++) {
                graphchi_edge<EdgeDataType> * e = vertex.edge(i);
                min_component_id = std::min(e->get_data().neighbor_label(vertex.id(), e->vertex_id()), min_component_id);
            }
            
            
            /* Set component ids and schedule neighbors */
            for(int i=0; i < vertex.num_edges(); i++) {
                graphchi_edge<EdgeDataType> * e = vertex.edge(i);
                bidirectional_component_weight edata = e->get_data();
                
                if (edata.my_label(vertex.id(), e->vertex_id()) != min_component_id) {
                    edata.my_label(vertex.id(), e->vertex_id()) = min_component_id;
                    e->set_data(edata);
                    
                }
            }
        } else {
            bool counted_as_contracting = false;
            for(int i=0; i < vertex.num_inedges(); i++) { // only in neighbor counts
                graphchi_edge<EdgeDataType> * e = vertex.inedge(i);
                bidirectional_component_weight edata = e->get_data();
                num_agree += edata.labels_agree();
                num_disagree += !edata.labels_agree();
                if (edata.labels_agree() && !counted_as_contracting) {
                    contracted++;
                    counted_as_contracting = true;
                }
                
                if (gcontext.iteration > 40 && !edata.labels_agree()) {
                    std::cout << "Disagree: " << vertex.id() << ":" << edata.my_label(vertex.id(), e->vertex_id()) << " != " << e->vertex_id() << ":" << edata.neighbor_label(vertex.id(), e->vertex_id()) << std::endl;
                }
            }
        }
        
        
    }
    /**
     * Called before an iteration starts.
     */
    void before_iteration(int iteration, graphchi_context &info) {
        num_agree = num_disagree = contracted = 0;
    }
    
    /**
     * Called after an iteration has finished.
     */
    void after_iteration(int iteration, graphchi_context &ginfo) {
        if (iteration % 2 == 1) {
            std::cout << "STATUS ON PROPAGATION ITERATION: " << iteration / 2 << " agree: " << num_agree << " disagree: " << num_disagree
            << " vertices contracting: " << contracted << std::endl;
            if (iteration / 2 == 0) contracted_after1 = contracted * 1.0 / ginfo.nvertices;
            if (iteration / 2 == 1) contracted_after2 = contracted * 1.0 / ginfo.nvertices;
            
            if (num_disagree == 0) {
                ginfo.set_last_iteration(ginfo.iteration);
                finish_iterations = iteration / 2;
                std::cout << "Converged in " << iteration / 2 <<  " iterations" << std::endl;
            }
        }
        
    }
    
    /**
     * Called before an execution interval is started.
     */
    void before_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &ginfo) {
    }
    
    /**
     * Called after an execution interval has finished.
     */
    void after_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &ginfo) {
    }
    
};

int main(int argc, const char ** argv) {
    
    /* GraphChi initialization will read the command line
     arguments and the configuration file. */
    graphchi_init(argc, argv);
    
    /* Metrics object for keeping track of performance counters
     and other information. Currently required. */
    metrics m("research-connected-components");
    
    /* Basic arguments for application */
    set_conf("execthreads", "1");
    assert(get_option_int("execthreads") == 1);
    std::string filename = get_option_string("file");  // Base filename
    int niters           = get_option_int("niters") * 2; // Number of iterations (odd iterations count agreements)
    bool scheduler       = false;    // Always run with scheduler
    
    
    char contr_log_fname[255];
    sprintf(contr_log_fname, "%s.contraction_log.txt", filename.c_str());
    
    FILE * contrlog = fopen(contr_log_fname, "a");
    
    contracted_after1 = contracted_after2 = 0;
    
    /* Process input file - and delete previous ones*/
    bool existed = false;
    int nshards             = (int) convert_if_notexists<EdgeDataType, EdgeDataType>(filename, get_option_string("nshards", "auto"), existed);
   
    
    /* Run */
    ResearchCC program;
    graphchi_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m);
    
    if (existed) {
        /* Clear edge values before run */
        engine.set_initialize_edges_before_run(true);
    }
    
    engine.run(program, niters);
    
    /* Report execution metrics */
    // metrics_report(m);
    fprintf(contrlog, "%d,%lf,%lf\n", finish_iterations, contracted_after1, contracted_after2);
    fflush(contrlog);
    
    fclose(contrlog);
    return 0;
}

