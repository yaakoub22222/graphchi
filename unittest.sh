
# Following command makes the unittest.sh fail if any of the
# commands fail.
set -e

function display_name {
  echo "****************************************************************************"
  echo -n "*******************"
  echo -n $1
  echo "**********************"
  echo "****************************************************************************"
}


rm -f smallnetflix_mm.*
display_name "TESTING BASELINE"
  ./toolkits/collaborative_filtering/baseline --training=smallnetflix_mm --validation=smallnetflix_mm --minval=1 --maxval=5 --quiet=1  --algorithm=user_mean
display_name "TESTING ALS"
 ./toolkits/collaborative_filtering/als --training=smallnetflix_mm --validation=smallnetflix_mme --lambda=0.065 --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING ALS - RMSE VALIDATION STOP"
 ./toolkits/collaborative_filtering/als --training=smallnetflix_mm --validation=smallnetflix_mme --lambda=0.1 --minval=1 --maxval=5 --max_iter=100 --quiet=1 --halt_on_rmse_increase=3
display_name "TESTING ALS SERIALIZATION"
 ./toolkits/collaborative_filtering/als --training=smallnetflix_mm --validation=smallnetflix_mme --lambda=0.065 --minval=1 --maxval=5 --max_iter=6 --quiet=1 --load_factors_from_file=1
display_name "TESTING SGD"
 ./toolkits/collaborative_filtering/sgd --training=smallnetflix_mm --validation=smallnetflix_mme --sgd_lambda=1e-4 --sgd_gamma=1e-4 --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING BIAS_SGD"
 ./toolkits/collaborative_filtering/biassgd --training=smallnetflix_mm --validation=smallnetflix_mme --biassgd_lambda=1e-4 --biassgd_gamma=1e-4 --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING BIAS_SGD SERIALIZATION"
 ./toolkits/collaborative_filtering/biassgd --training=smallnetflix_mm --validation=smallnetflix_mme --biassgd_lambda=1e-4 --biassgd_gamma=1e-4 --minval=1 --maxval=5 --max_iter=6 --quiet=1 --load_factors_from_file=1
display_name "TESTING SVD++"
 ./toolkits/collaborative_filtering/svdpp --training=smallnetflix_mm --validation=smallnetflix_mme --biassgd_lambda=1e-4 --biassgd_gamma=1e-4 --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING SVD++ SERIALIZATION"
 ./toolkits/collaborative_filtering/svdpp --training=smallnetflix_mm --validation=smallnetflix_mme --biassgd_lambda=1e-4 --biassgd_gamma=1e-4 --minval=1 --maxval=5 --max_iter=6 --quiet=1 --load_factors_from_file=1
display_name "TESTING NMF"
./toolkits/collaborative_filtering/nmf --training=reverse_netflix.mm --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING SVD"
./toolkits/collaborative_filtering/svd --training=smallnetflix_mm --nsv=3 --nv=5 --max_iter=5 --quiet=1 --tol=1e-1
display_name "TESTING SVD-ONESIDED"
./toolkits/collaborative_filtering/svd_onesided --training=smallnetflix_mm --nsv=3 --nv=5 --max_iter=5 --quiet=1 --tol=1e-1
display_name "TESTING RBM"
./toolkits/collaborative_filtering/rbm --training=smallnetflix_mm --validation=smallnetflix_mme --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING WALS"  
rm -f time_smallnetflix.* time_smallnetflixe.*
./toolkits/collaborative_filtering/wals --training=time_smallnetflix --validation=time_smallnetflixe --lambda=0.065 --minval=1 --maxval=5 --max_iter=6 --K=27 --quiet=1
display_name "TESTING ALS-TENSOR"  
./toolkits/collaborative_filtering/als_tensor --training=time_smallnetflix --validation=time_smallnetflixe --lambda=0.065 --minval=1 --maxval=5 --max_iter=6 --K=27 --quiet=1
rm -fR time_smallnetflix.*
display_name "TESTING TIME-SVD++"
./toolkits/collaborative_filtering/timesvdpp --training=time_smallnetflix --validation=time_smallnetflixe --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING LIBFM"
./toolkits/collaborative_filtering/libfm --training=time_smallnetflix --validation=time_smallnetflixe --minval=1 --maxval=5 --max_iter=6 --quiet=1
display_name "TESTING BIAS_SGD2 - LOGISTIC LOSS"
./toolkits/collaborative_filtering/biassgd2 --training=smallnetflix_mm --minval=1 --maxval=5 --validation=smallnetflix_mme --biassgd_gamma=1e-2 --biassgd_lambda=1e-2 --max_iter=6 --quiet=1 --loss=logistic --biassgd_step_dec=0.99999
display_name "TESTING BIAS_SGD2 - ABS LOSS"
./toolkits/collaborative_filtering/biassgd2 --training=smallnetflix_mm --minval=1 --maxval=5 --validation=smallnetflix_mme --biassgd_gamma=1e-2 --biassgd_lambda=1e-2 --max_iter=6 --quiet=1 --loss=abs --biassgd_step_dec=0.99999
display_name "TESTING BIAS_SGD2 - SQUARE LOSS"
./toolkits/collaborative_filtering/biassgd2 --training=smallnetflix_mm --minval=1 --maxval=5 --validation=smallnetflix_mme --biassgd_gamma=1e-2 --biassgd_lambda=1e-2 --max_iter=6 --quiet=1 --loss=square --biassgd_step_dec=0.99999
display_name "TESTING PMF"
 ./toolkits/collaborative_filtering/pmf --training=smallnetflix_mm --validation=smallnetflix_mme --quiet=1 --minval=1 --maxval=5 --max_iter=10 --pmf_burn_in=5
display_name "TESTING ITEMCF"
./toolkits/collaborative_filtering/itemcf --training=smallnetflix_mm --nshards=1 --quiet=1 --K=10
display_name "TESTING ITEMCF - AIOLLI ASYM COST"
./toolkits/collaborative_filtering/itemcf --training=smallnetflix_mm --nshards=1 --quiet=1 --distance=3 --K=10