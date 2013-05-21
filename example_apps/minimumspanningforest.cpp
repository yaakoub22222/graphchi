
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
 * Minimum spanning forest based on Boruvska steps.
 */

#define GRAPHCHI_DISABLE_COMPRESSION

#include <string>

#include "graphchi_basic_includes.hpp"

using namespace graphchi;




#define MAX_VIDT 0xffffffff

struct bidirectional_component_weight {
    vid_t smaller_component;
    vid_t larger_component;
    bool in_mst;
    float weight;
    
    bidirectional_component_weight() {
        smaller_component = larger_component = MAX_VIDT;
        in_mst = false;
        weight = 0.0f;
    }
    
    
    vid_t & neighbor_label(vid_t myid, vid_t nbid) {
        if (myid < nbid) {
            return larger_component;
        } else {
            return smaller_component;
        }
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

static void parse(bidirectional_component_weight &x, const char * s) {
    x.smaller_component = MAX_VIDT;
    x.larger_component = MAX_VIDT;
    x.in_mst = false;
    x.weight = (float) atof(s);
    std::cout << x.weight << std::endl;
}


/**
 * Type definitions. Remember to create suitable graph shards using the
 * Sharder-program. 
 */
typedef vid_t VertexDataType;
typedef bidirectional_component_weight EdgeDataType;

graphchi_engine<VertexDataType, EdgeDataType> * gengine;


size_t MST_OUTPUT;
size_t CONTRACTED_GRAPH_OUTPUT;

/**
 * GraphChi programs need to subclass GraphChiProgram<vertex-type, edge-type> 
 * class. The main logic is usually in the update function.
 */
struct BoruvskaStep : public GraphChiProgram<VertexDataType, EdgeDataType> {
    
    
    /**
     *  Vertex update function. Note: we assume fresh edge values.
     */
    void update(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, graphchi_context &gcontext) {
        
        if (vertex.num_edges() == 0) {
            return;
        }
        
        float min_edge_weight = 1e30f;
        int min_edge_idx = 0;
        
        // TODO: replace with reductions
        /* Get minimum edge */
        for(int i=0; i < vertex.num_edges(); i++) {
            float w = vertex.edge(i)->get_data().weight;
            if (w < min_edge_weight) {
                min_edge_idx = i;
                min_edge_weight = w;
            }
        }
        
        // On first iteration output MST edges
        if (gcontext.iteration == 0) {
            if (!vertex.edge(min_edge_idx)->get_data().in_mst) {
                bidirectional_component_weight edata = vertex.edge(min_edge_idx)->get_data();
                edata.in_mst = true;
                vertex.edge(min_edge_idx)->set_data(edata);
            }
        }
        
        /* Get my component id. It is the minimum label of a neighbor via a mst edge (or my own id) */
        vid_t min_component_id = vertex.id();
        for(int i=0; i < vertex.num_edges(); i++) {
            graphchi_edge<EdgeDataType> * e = vertex.edge(i);
            if (e->get_data().in_mst) {
                min_component_id = std::min(e->get_data().neighbor_label(vertex.id(), e->vertex_id()), min_component_id);
            }
        }
        
        /* Set component ids and schedule neighbors */
        if (min_component_id != vertex.get_data()) {
            vertex.set_data(min_component_id);    
            for(int i=0; i < vertex.num_edges(); i++) {
                graphchi_edge<EdgeDataType> * e = vertex.edge(i);
                bidirectional_component_weight edata = e->get_data();
                edata.my_label(vertex.id(), e->vertex_id()) = min_component_id;
                e->set_data(edata);
                 
                /* Schedule neighbor is connected by MST edge and neighbor has not updated yet */
                if (edata.in_mst) {
                    if (e->get_data().neighbor_label(vertex.id(), e->vertex_id()) != min_component_id) {
                        gcontext.scheduler->add_task(e->vertex_id());
                    }
                }
            }
        }
    }
    
    /**
     * Called before an iteration starts.
     */
    void before_iteration(int iteration, graphchi_context &gcontext) {
        logstream(LOG_INFO) << "Start iteration " << iteration << ", scheduled tasks=" << gcontext.scheduler->num_tasks() << std::endl;
    }
    
    void after_iteration(int iteration, graphchi_context &gcontext) {}
    void before_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &gcontext) {}
    void after_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &gcontext) {}
    
};


struct BoruvskaStep : public GraphChiProgram<VertexDataType, EdgeDataType> {
    
    
    /**
     *  Vertex update function. Note: we assume fresh edge values.
     */
    void update(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, graphchi_context &gcontext) {
        
        if (vertex.num_edges() == 0) {
            // Ugly
            gengine->output(MST_OUTPUT)->output_edge(vertex.id(), vertex.id());
            return;
        } 
        
        for(int i=0; i < vertex.num_edges(); i++) {
            graphchi_edge<EdgeDataType> * e = vertex.edge(i);
            bidirectional_component_weight edata = e->get_data();

            if (e->get_data().in_mst && edata.labels_agree()) {
                min_component_id = std::min(e->get_data().neighbor_label(vertex.id(), e->vertex_id()), min_component_id);
            }
        }
    }
    
    /**
     * Called before an iteration starts.
     */
    void before_iteration(int iteration, graphchi_context &gcontext) {
        logstream(LOG_INFO) << "Contraction: Start iteration " << iteration << ", scheduled tasks=" << gcontext.scheduler->num_tasks() << std::endl;
    }
    
    void after_iteration(int iteration, graphchi_context &gcontext) {}
    void before_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &gcontext) {}
    void after_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &gcontext) {}
    
};


int main(int argc, const char ** argv) {
    /* GraphChi initialization will read the command line 
     arguments and the configuration file. */
    graphchi_init(argc, argv);
    
    /* Metrics object for keeping track of performance counters
     and other information. Currently required. */
    metrics m("minimum-spanning-forest");
    
    /* Basic arguments for application */
    std::string filename = get_option_string("file");  // Base filename
    bool scheduler       = true; // Whether to use selective scheduling
    
    /* Detect the number of shards or preprocess an input to create them */
    int nshards          = get_option_int("nshards", 10);
    delete_shards<EdgeDataType>(filename, nshards);

    convert_if_notexists<EdgeDataType>(filename, get_option_string("nshards", "10"));
    
    /* Step 1: Run boruvska step */
    BoruvskaStep boruvska ;
    graphchi_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m); 
    engine.set_reset_vertexdata(true);
    gengine = &engine;
    
    engine.run(boruvska, 3);
    
    
    /* Step 2: Run contraction */
    
    /* Initialize output */
    basic_text_output<VertexDataType, EdgeDataType> mstout(filename + ".mst", "\t");
    sharded_graph_output<VertexDataType, EdgeDataType> shardedout(filename + "i0", engine.get_intervals());
    MST_OUTPUT = engine.add_output(&mstout);
    CONTRACTED_GRAPH_OUTPUT = engine.add_output(&shardedout);
    
    ContractionStep contraction;
    engine.set_reset_vertexdata(false);
    engine.run(contraction, 1);
    
    /* Report execution metrics */
    metrics_report(m);
    return 0;
}