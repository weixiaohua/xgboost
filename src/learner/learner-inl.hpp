#ifndef XGBOOST_LEARNER_LEARNER_INL_HPP_
#define XGBOOST_LEARNER_LEARNER_INL_HPP_
/*!
 * \file learner-inl.hpp
 * \brief learning algorithm 
 * \author Tianqi Chen
 */
#include <algorithm>
#include <vector>
#include <utility>
#include <string>
#include "./objective.h"
#include "./evaluation.h"
#include "../gbm/gbm.h"

namespace xgboost {
/*! \brief namespace for learning algorithm */
namespace learner {
/*! 
 * \brief learner that takes do gradient boosting on specific objective functions
 *  and do training and prediction
 */
template<typename FMatrix>
class BoostLearner {
 public:
  BoostLearner(void) {
    obj_ = NULL;
    gbm_ = NULL;
    name_obj_ = "reg:linear";
    name_gbm_ = "gbtree";
  }
  ~BoostLearner(void) {
    if (obj_ != NULL) delete obj_;
    if (gbm_ != NULL) delete gbm_;
  }
  /*!
   * \brief add internal cache space for mat, this can speedup prediction for matrix,
   *        please cache prediction for training and eval data
   *    warning: if the model is loaded from file from some previous training history
   *             set cache data must be called with exactly SAME 
   *             data matrices to continue training otherwise it will cause error
   * \param mats array of pointers to matrix whose prediction result need to be cached
   */          
  inline void SetCacheData(const std::vector<DMatrix<FMatrix>*>& mats) {
    // estimate feature bound
    unsigned num_feature = 0;
    // assign buffer index
    size_t buffer_size = 0;
    utils::Assert(cache_.size() == 0, "can only call cache data once");
    for (size_t i = 0; i < mats.size(); ++i) {
      bool dupilicate = false;
      for (size_t j = 0; j < i; ++j) {
        if (mats[i] == mats[j]) dupilicate = true;
      }
      if (dupilicate) continue;
      // set mats[i]'s cache learner pointer to this
      mats[i]->cache_learner_ptr_ = this;
      cache_.push_back(CacheEntry(mats[i], buffer_size, mats[i]->info.num_row));
      buffer_size += mats[i]->info.num_row;
      num_feature = std::max(num_feature, static_cast<unsigned>(mats[i]->info.num_col));
    }
    char str_temp[25];
    if (num_feature > mparam.num_feature) {
      snprintf(str_temp, sizeof(str_temp), "%u", num_feature);
      this->SetParam("bst:num_feature", str_temp);
    }
    snprintf(str_temp, sizeof(str_temp), "%lu", buffer_size);
    this->SetParam("num_pbuffer", str_temp);
    if (!silent) {
      printf("buffer_size=%ld\n", buffer_size);
    }
  }
  /*!
   * \brief set parameters from outside
   * \param name name of the parameter
   * \param val  value of the parameter
   */
  inline void SetParam(const char *name, const char *val) {
    if (!strcmp(name, "silent")) silent = atoi(val);
    if (!strcmp(name, "eval_metric")) evaluator_.AddEval(val);
    if (gbm_ == NULL) {
      if (!strcmp(name, "objective")) name_obj_ = val;
      if (!strcmp(name, "booster")) name_gbm_ = val;
      mparam.SetParam(name, val);
    }
    cfg_.push_back(std::make_pair(std::string(name), std::string(val)));
  }
  /*!
   * \brief initialize the model
   */
  inline void InitModel(void) {
    this->InitObjGBM();
    // adapt the base score
    mparam.base_score = obj_->ProbToMargin(mparam.base_score);
    gbm_->InitModel();
  }
  /*!
   * \brief load model from stream
   * \param fi input stream
   */
  inline void LoadModel(utils::IStream &fi) {
    utils::Check(fi.Read(&mparam, sizeof(ModelParam)) != 0,
                 "BoostLearner: wrong model format");
    utils::Check(fi.Read(&name_obj_), "BoostLearner: wrong model format");
    utils::Check(fi.Read(&name_gbm_), "BoostLearner: wrong model format");
    // delete existing gbm if any
    if (obj_ != NULL) delete obj_;
    if (gbm_ != NULL) delete gbm_;
    this->InitObjGBM();
    gbm_->LoadModel(fi);
  }
  /*!
   * \brief load model from file
   * \param fname file name
   */
  inline void LoadModel(const char *fname) {
    utils::FileStream fi(utils::FopenCheck(fname, "rb"));
    this->LoadModel(fi);
    fi.Close();
  }
  inline void SaveModel(utils::IStream &fo) const {
    fo.Write(&mparam, sizeof(ModelParam));
    fo.Write(&name_obj_);
    fo.Write(&name_gbm_);
    gbm_->SaveModel(fo);
  }
  /*!
   * \brief save model into file
   * \param fname file name
   */
  inline void SaveModel(const char *fname) const {
    utils::FileStream fo(utils::FopenCheck(fname, "wb"));
    this->SaveModel(fo);
    fo.Close();
  }  
  /*!
   * \brief update the model for one iteration
   * \param iter current iteration number
   * \param p_train pointer to the data matrix
   */
  inline void UpdateOneIter(int iter, DMatrix<FMatrix> *p_train) {
    this->PredictRaw(preds_, *p_train);
    obj_->GetGradient(preds_, p_train->info, iter, &gpair_);
    gbm_->DoBoost(gpair_, p_train->fmat, p_train->info.root_index);
  }
  /*!
   * \brief evaluate the model for specific iteration
   * \param iter iteration number
   * \param evals datas i want to evaluate
   * \param evname name of each dataset
   * \return a string corresponding to the evaluation result
   */
  inline std::string EvalOneIter(int iter,
                                 const std::vector<const DMatrix<FMatrix>*> &evals,
                                 const std::vector<std::string> &evname) {
    std::string res;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "[%d]", iter);
    res = tmp;
    for (size_t i = 0; i < evals.size(); ++i) {
      this->PredictRaw(*evals[i], &preds_);
      obj_->EvalTransform(&preds_);
      res += evaluator_.Eval(evname[i].c_str(), preds_, evals[i]->info);
    }
    return res;
  }
  /*!
   * \brief simple evaluation function, with a specified metric
   * \param data input data
   * \param metric name of metric
   * \return a pair of <evaluation name, result>
   */
  std::pair<std::string, float> Evaluate(const DMatrix<FMatrix> &data, std::string metric) {
    if (metric == "auto") metric = obj_->DefaultEvalMetric();
    IEvaluator *ev = CreateEvaluator(metric.c_str());
    this->PredictRaw(data, &preds_);
    obj_->EvalTransform(&preds_);
    float res = ev->Eval(preds_, data.info);
    delete ev;
    return std::make_pair(metric, res);
  }
  /*!
   * \brief get prediction
   * \param data input data
   * \param out_preds output vector that stores the prediction
   */
  inline void Predict(const DMatrix<FMatrix> &data,
                      std::vector<float> *out_preds) const {
    this->PredictRaw(data, out_preds);
    obj_->PredTransform(out_preds);
  }

 protected:
  /*! 
   * \brief initialize the objective function and GBM, 
   * if not yet done
   */
  inline void InitObjGBM(void) {
    if (obj_ != NULL) return;
    utils::Assert(gbm_ == NULL, "GBM and obj should be NULL");
    obj_ = CreateObjFunction(name_obj_.c_str());
    gbm_ = gbm::CreateGradBooster<FMatrix>(name_gbm_.c_str());
    for (size_t i = 0; i < cfg_.size(); ++i) {
      obj_->SetParam(cfg_[i].first.c_str(), cfg_[i].second.c_str());
      gbm_->SetParam(cfg_[i].first.c_str(), cfg_[i].second.c_str());
    }
    evaluator_.AddEval(obj_->DefaultEvalMetric());
  }
  /*! 
   * \brief get un-transformed prediction
   * \param data training data matrix
   * \param out_preds output vector that stores the prediction
   */
  inline void PredictRaw(const DMatrix<FMatrix> &data,
                         std::vector<float> *out_preds) {
    gbm_->Predict(data.fmat, this->FindBufferOffset(data),
                  data.info, out_preds);
  }

  /*! \brief training parameter for regression */
  struct ModelParam{
    /* \brief global bias */
    float base_score;
    /* \brief number of features  */
    unsigned num_feature;
    /* \brief number of class, if it is multi-class classification  */
    int num_class;
    /*! \brief reserved field */
    int reserved[32];
    /*! \brief constructor */
    ModelParam(void) {
      base_score = 0.5f;
      num_feature = 0;
      num_class = 0;
      memset(reserved, 0, sizeof(reserved));
    }
    /*!
     * \brief set parameters from outside
     * \param name name of the parameter
     * \param val value of the parameter
     */
    inline void SetParam(const char *name, const char *val) {
      if (!strcmp("base_score", name)) base_score = static_cast<float>(atof(val));
      if (!strcmp("num_class", name)) num_class = atoi(val);
      if (!strcmp("bst:num_feature", name)) num_feature = atoi(val);
    }
  };
  // data fields
  // silent during training
  int silent;
  // evaluation set
  EvalSet evaluator_;
  // model parameter
  ModelParam   mparam;
  // gbm model that back everything
  gbm::IGradBooster<FMatrix> *gbm_;
  // name of gbm model used for training
  std::string name_gbm_;
  // objective fnction
  IObjFunction *obj_;
  // name of objective function
  std::string name_obj_;
  // configurations
  std::vector< std::pair<std::string, std::string> > cfg_;
  // temporal storages for prediciton
  std::vector<float> preds_;
  // gradient pairs
  std::vector<bst_gpair> gpair_;

 private:
  // cache entry object that helps handle feature caching
  struct CacheEntry {
    const DMatrix<FMatrix> *mat_;
    size_t buffer_offset_;
    size_t num_row_;
    CacheEntry(const DMatrix<FMatrix> *mat, size_t buffer_offset, size_t num_row)
        :mat_(mat), buffer_offset_(buffer_offset), num_row_(num_row) {}
  };
  // find internal bufer offset for certain matrix, if not exist, return -1
  inline int64_t FindBufferOffset(const DMatrix<FMatrix> &mat) const {
    for (size_t i = 0; i < cache_.size(); ++i) {
      if (cache_[i].mat_ == &mat && mat.cache_learner_ptr_ == this) {
        if (cache_[i].num_row_ == mat.num_row) {
          return cache_[i].buffer_offset_;
        }
      }
    }
    return -1;
  }
  // data structure field
  /*! \brief the entries indicates that we have internal prediction cache */
  std::vector<CacheEntry> cache_;
};
}  // namespace learner
}  // namespace xgboost
#endif  // XGBOOST_LEARNER_LEARNER_INL_HPP_
