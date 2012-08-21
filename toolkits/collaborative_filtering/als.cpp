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
 * Matrix factorizatino with the Alternative Least Squares (ALS) algorithm.
 * This code is based on GraphLab's implementation of ALS by Joey Gonzalez
 * and Danny Bickson (CMU). A good explanation of the algorithm is 
 * given in the following paper:
 *    Large-Scale Parallel Collaborative Filtering for the Netflix Prize
 *    Yunhong Zhou, Dennis Wilkinson, Robert Schreiber and Rong Pan
 *    http://www.springerlink.com/content/j1076u0h14586183/
 *
 * Faster version of ALS, which stores latent factors of vertices in-memory.
 * Thus, this version requires more memory. See the version "als_edgefactors"
 * for a low-memory implementation.
 *
 *
 * In the code, we use movie-rating terminology for clarity. This code has been
 * tested with the Netflix movie rating challenge, where the task is to predict
 * how user rates movies in range from 1 to 5.
 *
 * This code is has integrated preprocessing, 'sharding', so it is not necessary
 * to run sharder prior to running the matrix factorization algorithm. Input
 * data must be provided in the Matrix Market format (http://math.nist.gov/MatrixMarket/formats.html).
 *
 * ALS uses free linear algebra library 'Eigen'. See Readme_Eigen.txt for instructions
 * how to obtain it.
 *
 * At the end of the processing, the two latent factor matrices are written into files in 
 * the matrix market format. 
 *
 * @section USAGE
 *
 * bin/example_apps/matrix_factorization/als_edgefactors file <matrix-market-input> niters 5
 *
 * 
 */



#include <string>
#include <algorithm>

#include "graphchi_basic_includes.hpp"

/* ALS-related classes are contained in als.hpp */
#include "als.hpp"

using namespace graphchi;


/**
 * Type definitions. Remember to create suitable graph shards using the
 * Sharder-program. 
 */
typedef vertex_data VertexDataType;
typedef float EdgeDataType;  // Edges store the "rating" of user->movie pair

graphchi_engine<VertexDataType, EdgeDataType> * pengine = NULL; 
std::vector<vertex_data> latent_factors_inmem;


#include "rmse.hpp"

/** compute a missing value based on ALS algorithm */
float als_predict(const vertex_data& user, 
                const vertex_data& movie, 
                const float rating, 
                double & prediction){
 

  prediction = user.dot(movie);
  //truncate prediction to allowed values
  prediction = std::min((double)prediction, maxval);
  prediction = std::max((double)prediction, minval);
  //return the squared error
  float err = rating - prediction;
  assert(!std::isnan(err));
  return err*err; 
 
}




/**
 * GraphChi programs need to subclass GraphChiProgram<vertex-type, edge-type> 
 * class. The main logic is usually in the update function.
 */
struct ALSVerticesInMemProgram : public GraphChiProgram<VertexDataType, EdgeDataType> {


  // Helper
  virtual void set_latent_factor(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, vertex_data &fact) {
    vertex.set_data(fact); // Note, also stored on disk. This is non-optimal...
    latent_factors_inmem[vertex.id()] = fact;
  }

  /**
   * Called before an iteration starts.
   */
  void before_iteration(int iteration, graphchi_context &gcontext) {
    if (iteration == 0) {
      latent_factors_inmem.resize(gcontext.nvertices); // Initialize in-memory vertices.
      assert(M > 0 && N > 0);
      max_left_vertex = M-1;
      max_right_vertex = M+N-1;
    }
  }

  /**
   *  Vertex update function.
   */
  void update(graphchi_vertex<VertexDataType, EdgeDataType> &vertex, graphchi_context &gcontext) {
    if (gcontext.iteration == 0) {

    } else {
      mat XtX(NLATENT, NLATENT); 
      XtX.setZero();
      vec Xty(NLATENT);
      Xty.setZero();

      // Compute XtX and Xty (NOTE: unweighted)
      for(int e=0; e < vertex.num_edges(); e++) {
        float observation = vertex.edge(e)->get_data();                
        vertex_data & nbr_latent = latent_factors_inmem[vertex.edge(e)->vertex_id()];
        for(int i=0; i<NLATENT; i++) {
          Xty(i) += nbr_latent.d[i] * observation;
          for(int j=i; j < NLATENT; j++) {
            XtX(j,i) += nbr_latent.d[i] * nbr_latent.d[j];
          }
        }
      }

      // Symmetrize
      for(int i=0; i <NLATENT; i++)
        for(int j=i + 1; j< NLATENT; j++) XtX(i,j) = XtX(j,i);

      // Diagonal
      for(int i=0; i < NLATENT; i++) XtX(i,i) += (lambda) * vertex.num_edges();

      // Solve the least squares problem with eigen using Cholesky decomposition
      vec veclatent = XtX.ldlt().solve(Xty);

      // Convert to plain doubles (this is useful because now the output data by GraphCHI
      // is plain binary double matrix that can be read, for example, by Matlab).
      vertex_data newlatent;
      memcpy(newlatent.d, &veclatent[0], sizeof(double)*NLATENT);
      newlatent.rmse = 0; 

      bool compute_rmse = (vertex.num_outedges() > 0);
      if (compute_rmse) { // Compute RMSE only on "right side" of bipartite graph
        for(int e=0; e < vertex.num_edges(); e++) {        
          // Compute RMSE
          double observation = vertex.edge(e)->get_data();
          vertex_data & nbr_latent =  latent_factors_inmem[vertex.edge(e)->vertex_id()];
          double prediction = nbr_latent.dot(newlatent);
          prediction = std::min(maxval, prediction);
          prediction = std::max(minval, prediction);
          newlatent.rmse += (prediction - observation) * (prediction - observation);                

        }

      }

      set_latent_factor(vertex, newlatent); 
    }

  }



  /**
   * Called after an iteration has finished.
   */
  void after_iteration(int iteration, graphchi_context &gcontext) {
    rmse = 0;
#pragma omp parallel for reduction(+:rmse)
    for (uint i=0; i< max_left_vertex; i++){
      rmse += latent_factors_inmem[i].rmse;
    }
    logstream(LOG_INFO)<<"Training RMSE: " << sqrt(rmse/pengine->num_edges()) << std::endl;
    validation_rmse(&als_predict);
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

struct  MMOutputter{
  FILE * outf;
  MMOutputter(std::string fname, uint start, uint end, std::string comment)  {
    assert(start < end);
    MM_typecode matcode;
    set_matcode(matcode);     
    outf = fopen(fname.c_str(), "w");
    assert(outf != NULL);
    mm_write_banner(outf, matcode);
    if (comment != "")
      fprintf(outf, "%%%s\n", comment.c_str());
    mm_write_mtx_array_size(outf, end-start, NLATENT); 
    for (uint i=start; i < end; i++)
      for(int j=0; j < NLATENT; j++) {
        fprintf(outf, "%1.12e\n", latent_factors_inmem[i].d[j]);
    }
  }

  ~MMOutputter() {
    if (outf != NULL) fclose(outf);
  }

};


void output_als_result(std::string filename, vid_t numvertices, vid_t max_left_vertex) {
  MMOutputter mmoutput_left(filename + "_U.mm", 0, max_left_vertex + 1, "This file contains ALS output matrix U. In each row NLATENT factors of a single user node.");
  MMOutputter mmoutput_right(filename + "_V.mm", max_left_vertex +1 ,numvertices, "This file contains ALS  output matrix V. In each row NLATENT factors of a single item node.");
  logstream(LOG_INFO) << "ALS output files (in matrix market format): " << filename << "_U.mm" <<
                                                                             ", " << filename + "_V.mm " << std::endl;
}

int main(int argc, const char ** argv) {


  logstream(LOG_WARNING)<<"GraphChi Collaborative filtering library is written by Danny Bickson (c). Send any "
    " comments or bug reports to danny.bickson@gmail.com " << std::endl;
  /* GraphChi initialization will read the command line 
     arguments and the configuration file. */
  graphchi_init(argc, argv);

  /* Metrics object for keeping track of performance counters
     and other information. Currently required. */
  metrics m("als-inmemory-factors");

  /* Basic arguments for application. NOTE: File will be automatically 'sharded'. */
  training = get_option_string("training");    // Base filename
  validation = get_option_string("validation", "");
  test = get_option_string("test", "");

  if (validation == "")
    validation += training + "e";  
  if (test == "")
    test += training + "t";

  int niters    = get_option_int("niters", 6);  // Number of iterations
  maxval        = get_option_float("maxval", 1e100);
  minval        = get_option_float("minval", -1e100);
  lambda        = get_option_float("lambda", 0.065);

  bool scheduler       = false;                        // Selective scheduling not supported for now.

  /* Preprocess data if needed, or discover preprocess files */
  int nshards = convert_matrixmarket<float>(training);

  /* Run */
  ALSVerticesInMemProgram program;
  graphchi_engine<VertexDataType, EdgeDataType> engine(training, nshards, scheduler, m); 
  engine.set_modifies_inedges(false);
  engine.set_modifies_outedges(false);
  pengine = &engine;
  engine.run(program, niters);

  m.set("train_rmse", rmse);
  m.set("latent_dimension", NLATENT);

  /* Output latent factor matrices in matrix-market format */
  vid_t numvertices = engine.num_vertices();
  assert(numvertices == max_right_vertex + 1); // Sanity check
  output_als_result(training, numvertices, max_left_vertex);
  test_predictions(&als_predict);    

  /* Report execution metrics */
  metrics_report(m);
  return 0;
}
