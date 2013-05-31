
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
 * Template for GraphChi applications. To create a new application, duplicate
 * this template.
 */

#define GRAPHCHI_DISABLE_COMPRESSION

#include <string>
#include <climits>
#include "graphchi_basic_includes.hpp"

using namespace graphchi;
using namespace std;
/**
 * Type definitions. Remember to create suitable graph shards using the
 * Sharder-program. 
 */
typedef int VertexDataType;
typedef int EdgeDataType;

/**
 * GraphChi programs need to subclass GraphChiProgram<vertex-type, edge-type> 
 * class. The main logic is usually in the update function.
 */
struct BFSProgram : public GraphChiProgram<VertexDataType, EdgeDataType> {
  /**
   *  Vertex update function.
   */
  uint root;
  BFSProgram(uint _root) : root(_root) {}
  void update(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, graphchi_context &gcontext) {
    if (gcontext.iteration == 0) {
      vertex.set_data(INT_MAX);
      for(int i=0; i < vertex.num_outedges(); i++) {
	vertex.outedge(i)->set_data(INT_MAX); 
      }
    }
    else if(gcontext.iteration == 1){ //root places neighbors
      vertex.set_data(0);
      for(int i=0;i<vertex.num_edges();i++){
	vertex.edge(i)->set_data(0);
	gcontext.scheduler->add_task(vertex.edge(i)->vertex_id());
      }
    }
    else {
      //std::cout << "vertex " << vertex.id() << " "<<vertex.get_data()<<std::endl;	
      /* Do computation */ 
      //if(vertex.get_data() == -1) {
      int minLevel = INT_MAX;
      for(int i=0;i<vertex.num_edges();i++) {	      
	minLevel = min(minLevel,vertex.edge(i)->get_data());
      }
      if(minLevel < INT_MAX) {
	vertex.set_data(minLevel+1);
	for(int i=0;i<vertex.num_edges();i++)
	  if(vertex.edge(i)->get_data() == INT_MAX){
	    vertex.edge(i)->set_data(vertex.get_data());
	    /* Schedule neighbor for update */
	    gcontext.scheduler->add_task(vertex.edge(i)->vertex_id());
	    //cout<<vertex.get_data()<<" scheduled "<<vertex.edge(i)->vertex_id()<<endl;
	  }
      }
	// }
    }
  }
    
  /**
   * Called before an iteration starts.
   */
  void before_iteration(int iteration, graphchi_context &gcontext) {
  }
    
  /**
   * Called after an iteration has finished.
   */
  void after_iteration(int iteration, graphchi_context &gcontext) {
    if(iteration == 0) gcontext.scheduler->add_task(root);
  }
    
  /**
   * Called before an execution interval is started.
   */
  void before_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &gcontext) {        
  }
    
  /**
   * Called after an execution interval has finished.
   */
  void after_exec_interval(vid_t window_st, vid_t window_en, graphchi_context &gcontext) {        
  }
};

int main(int argc, const char ** argv) {
  /* GraphChi initialization will read the command line 
     arguments and the configuration file. */
  graphchi_init(argc, argv);
  /* Metrics object for keeping track of performance counters
     and other information. Currently required. */
  metrics m("breadth first search");
    
  /* Basic arguments for application */
  std::string filename = get_option_string("file");  // Base filename
  int niters           = get_option_int("niters", 100); // Number of iterations
  bool scheduler       = true;
  int root = get_option_int("root",0);

  int nshards = atoi(get_option_string("nshards", "auto").c_str());

  delete_shards<EdgeDataType>(filename, nshards); 
  /* Detect the number of shards or preprocess an input to create them */
  char nshards_string[10];
  sprintf(nshards_string,"%d",nshards);
  nshards = convert_if_notexists<EdgeDataType>(filename,get_option_string("nshards",nshards_string));
   
  /* Run */
  BFSProgram program(root);
  graphchi_engine<VertexDataType, EdgeDataType> engine(filename, nshards, scheduler, m); 
  engine.set_reset_vertexdata(true);
  engine.run(program, niters);
    
  /* Report execution metrics */
  metrics_report(m);
  return 0;
}
