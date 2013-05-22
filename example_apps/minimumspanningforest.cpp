
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
    vid_t orig_src, orig_dst;
    bool in_mst;
    float weight;
    
    bidirectional_component_weight() {
        smaller_component = larger_component = MAX_VIDT;
        in_mst = false;
        weight = 0.0f;
        orig_src = orig_dst = 0;
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

class AcceptMinimum : public DuplicateEdgeFilter<bidirectional_component_weight> {
    bool acceptFirst(bidirectional_component_weight &first, bidirectional_component_weight &second) {
        return (first.weight < second.weight);
    }
};

static void parse(bidirectional_component_weight &x, const char * s) {
    x.smaller_component = MAX_VIDT;
    x.larger_component = MAX_VIDT;
    x.in_mst = false;
    x.weight = (float) atof(s);
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
            bidirectional_component_weight edata = vertex.edge(i)->get_data();

            float w = edata.weight;
            if (w < min_edge_weight) {
                min_edge_idx = i;
                min_edge_weight = w;
            }
            if (edata.orig_src == edata.orig_dst) {
                edata.orig_src = vertex.id();
                edata.orig_dst = vertex.edge(i)->vertex_id(); // Note: forgets direction
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
            for(int i=0; i < vertex.num_edges(); i++) {
                graphchi_edge<EdgeDataType> * e = vertex.edge(i);
                bidirectional_component_weight edata = e->get_data();
                
                if (edata.my_label(vertex.id(), e->vertex_id()) != min_component_id) {
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


struct ContractionStep : public GraphChiProgram<VertexDataType, EdgeDataType> {
    
    bool new_edges;
    
    ContractionStep() {
        new_edges = false;
    }
    
    /**
     *  Vertex update function. Note: we assume fresh edge values.
     */
    void update(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, graphchi_context &gcontext) {
        
        if (vertex.num_edges() == 0) {
            return;
        }
        for(int i=0; i < vertex.num_edges(); i++) {
            graphchi_edge<EdgeDataType> * e = vertex.edge(i);

            if (vertex.id() < e->vertex_id()) {
                // Vertex with smaller id has responsibility of outputing the MST edge
                // TODO: output original edge id
                
                bidirectional_component_weight edata = e->get_data();
                
                if (e->get_data().in_mst && edata.labels_agree()) {
                    gengine->output(MST_OUTPUT)->output_edge(edata.orig_src, edata.orig_dst, edata.weight);
                    
                } else if (!edata.labels_agree()) {
                    // Output the contracted edge
                    
                    vid_t a = edata.my_label(vertex.id(), e->vertex_id());
                    vid_t b = edata.neighbor_label(vertex.id(), e->vertex_id());
                    
                    edata.in_mst = false;
                    edata.smaller_component = edata.larger_component = MAX_VIDT;
                    
                    // Vary in/out edge for balance (not sure if required)
                    new_edges = true;
                    gengine->output(CONTRACTED_GRAPH_OUTPUT)->output_edge(a % 2 == 0 ? a : b,
                                                                          a % 2 == 0 ? b : a,
                                                                          edata);
                } else {
                    // Otherwise: discard the edge
                }
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

// As the number of edges is reduced every iteration by at least half, we can reduce the number
// of shards by two as well.
std::vector< std::pair<vid_t, vid_t> > halve_intervals(std::vector< std::pair<vid_t, vid_t> >  ints);
std::vector< std::pair<vid_t, vid_t> > halve_intervals(std::vector< std::pair<vid_t, vid_t> >  ints) {
    // Halve the intervals
    if (ints.size() == 1) {
        return ints;
    }
    std::vector< std::pair<vid_t, vid_t> > newints;
    for(int j=0; j < ints.size(); j += 2) {
        if (j + 1 < ints.size()) {
            newints.push_back(std::pair<vid_t, vid_t>(ints[j].first, ints[j + 1].second));
        } else {
            newints.push_back(std::pair<vid_t, vid_t>(ints[j].first, ints[j].second));

        }
    }
    return newints;
}


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
    
    
    for(int MSF_iteration=0; MSF_iteration < 100; MSF_iteration++) {
        logstream(LOG_INFO) << "MSF ITERATION " << MSF_iteration << std::endl;
        /* Step 1: Run boruvska step */
        BoruvskaStep boruvska ;
        graphchi_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m);
        engine.set_disable_vertexdata_storage();
        gengine = &engine;
        
        engine.run(boruvska, nshards > 1 ? 3 : 1000); // Only 3 iterations when not in-memory (one shard), otherwise run only 3 iters
        
        
        /* Step 2: Run contraction */
        /* Initialize output */
        basic_text_output<VertexDataType, EdgeDataType> mstout(filename + ".mst", "\t");
        
        
        std::string contractedname = filename + "C";
        std::vector< std::pair<vid_t, vid_t> > new_intervals = halve_intervals(engine.get_intervals());
        delete_shards<EdgeDataType>(contractedname, new_intervals.size());
        sharded_graph_output<VertexDataType, EdgeDataType> shardedout(contractedname, new_intervals, new AcceptMinimum());
        MST_OUTPUT = engine.add_output(&mstout);
        CONTRACTED_GRAPH_OUTPUT = engine.add_output(&shardedout);
        
        ContractionStep contraction;
        engine.set_disable_vertexdata_storage();
        engine.run(contraction, 1);
        
        if (contraction.new_edges == false) {
            logstream(LOG_INFO) << "MSF ready!" << std::endl;
            break;
        }
        
        nshards = shardedout.finish_sharding();
        filename = contractedname;
       
    }
    /* Report execution metrics */
    metrics_report(m);
    return 0;
}