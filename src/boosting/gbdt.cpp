#include "gbdt.h"

#include <LightGBM/utils/common.h>

#include <LightGBM/feature.h>
#include <LightGBM/objective_function.h>
#include <LightGBM/metric.h>

#include <ctime>

#include <sstream>
#include <chrono>
#include <string>
#include <vector>
#include <utility>

namespace LightGBM {

GBDT::GBDT()
  : train_score_updater_(nullptr),
  gradients_(nullptr), hessians_(nullptr),
  out_of_bag_data_indices_(nullptr), bag_data_indices_(nullptr),
  saved_model_size_(-1), num_used_model_(0) {
}

GBDT::~GBDT() {
  for (auto& tree_learner: tree_learner_){
    if (tree_learner != nullptr) { delete tree_learner; }
  }
  if (gradients_ != nullptr) { delete[] gradients_; }
  if (hessians_ != nullptr) { delete[] hessians_; }
  if (out_of_bag_data_indices_ != nullptr) { delete[] out_of_bag_data_indices_; }
  if (bag_data_indices_ != nullptr) { delete[] bag_data_indices_; }
  for (auto& tree : models_) {
    if (tree != nullptr) { delete tree; }
  }
  if (train_score_updater_ != nullptr) { delete train_score_updater_; }
  for (auto& score_tracker : valid_score_updater_) {
    if (score_tracker != nullptr) { delete score_tracker; }
  }
}

void GBDT::Init(const BoostingConfig* config, const Dataset* train_data, const ObjectiveFunction* object_function,
     const std::vector<const Metric*>& training_metrics) {
  gbdt_config_ = dynamic_cast<const GBDTConfig*>(config);
  iter_ = 0;
  saved_model_size_ = -1;
  max_feature_idx_ = 0;
  early_stopping_round_ = gbdt_config_->early_stopping_round;
  train_data_ = train_data;
  num_class_ = config->num_class;
  tree_learner_ = std::vector<TreeLearner*>(num_class_, nullptr);
  // create tree learner
  for (int i = 0; i < num_class_; ++i){
      tree_learner_[i] =
        TreeLearner::CreateTreeLearner(gbdt_config_->tree_learner_type, gbdt_config_->tree_config);
      // init tree learner
      tree_learner_[i]->Init(train_data_);
  }
  object_function_ = object_function;
  // push training metrics
  for (const auto& metric : training_metrics) {
    training_metrics_.push_back(metric);
  }
  // create score tracker
  train_score_updater_ = new ScoreUpdater(train_data_, num_class_);
  num_data_ = train_data_->num_data();
  // create buffer for gradients and hessians
  if (object_function_ != nullptr) {
    gradients_ = new score_t[num_data_ * num_class_];
    hessians_ = new score_t[num_data_ * num_class_];
  }

  // get max feature index
  max_feature_idx_ = train_data_->num_total_features() - 1;
  // get label index
  label_idx_ = train_data_->label_idx();
  // if need bagging, create buffer
  if (gbdt_config_->bagging_fraction < 1.0 && gbdt_config_->bagging_freq > 0) {
    out_of_bag_data_indices_ = new data_size_t[num_data_];
    bag_data_indices_ = new data_size_t[num_data_];
  } else {
    out_of_bag_data_cnt_ = 0;
    out_of_bag_data_indices_ = nullptr;
    bag_data_cnt_ = num_data_;
    bag_data_indices_ = nullptr;
  }
  // initialize random generator
  random_ = Random(gbdt_config_->bagging_seed);

}

void GBDT::AddDataset(const Dataset* valid_data,
         const std::vector<const Metric*>& valid_metrics) {
  if (iter_ > 0) {
    Log::Fatal("Cannot add validation data after training started");
  }
  // for a validation dataset, we need its score and metric
  valid_score_updater_.push_back(new ScoreUpdater(valid_data, num_class_));
  valid_metrics_.emplace_back();
  if (early_stopping_round_ > 0) {
    best_iter_.emplace_back();
    best_score_.emplace_back();
  }
  for (const auto& metric : valid_metrics) {
    valid_metrics_.back().push_back(metric);
    if (early_stopping_round_ > 0) {
      best_iter_.back().push_back(0);
      best_score_.back().push_back(kMinScore);
    }
  }
}


void GBDT::Bagging(int iter, const int curr_class) {
  // if need bagging
  if (out_of_bag_data_indices_ != nullptr && iter % gbdt_config_->bagging_freq == 0) {
    // if doesn't have query data
    if (train_data_->metadata().query_boundaries() == nullptr) {
      bag_data_cnt_ =
        static_cast<data_size_t>(gbdt_config_->bagging_fraction * num_data_);
      out_of_bag_data_cnt_ = num_data_ - bag_data_cnt_;
      data_size_t cur_left_cnt = 0;
      data_size_t cur_right_cnt = 0;
      // random bagging, minimal unit is one record
      for (data_size_t i = 0; i < num_data_; ++i) {
        double prob =
          (bag_data_cnt_ - cur_left_cnt) / static_cast<double>(num_data_ - i);
        if (random_.NextDouble() < prob) {
          bag_data_indices_[cur_left_cnt++] = i;
        } else {
          out_of_bag_data_indices_[cur_right_cnt++] = i;
        }
      }
    } else {
      // if have query data
      const data_size_t* query_boundaries = train_data_->metadata().query_boundaries();
      data_size_t num_query = train_data_->metadata().num_queries();
      data_size_t bag_query_cnt =
          static_cast<data_size_t>(num_query * gbdt_config_->bagging_fraction);
      data_size_t cur_left_query_cnt = 0;
      data_size_t cur_left_cnt = 0;
      data_size_t cur_right_cnt = 0;
      // random bagging, minimal unit is one query
      for (data_size_t i = 0; i < num_query; ++i) {
        double prob =
            (bag_query_cnt - cur_left_query_cnt) / static_cast<double>(num_query - i);
        if (random_.NextDouble() < prob) {
          for (data_size_t j = query_boundaries[i]; j < query_boundaries[i + 1]; ++j) {
            bag_data_indices_[cur_left_cnt++] = j;
          }
          cur_left_query_cnt++;
        } else {
          for (data_size_t j = query_boundaries[i]; j < query_boundaries[i + 1]; ++j) {
            out_of_bag_data_indices_[cur_right_cnt++] = j;
          }
        }
      }
      bag_data_cnt_ = cur_left_cnt;
      out_of_bag_data_cnt_ = num_data_ - bag_data_cnt_;
    }
    Log::Info("Re-bagging, using %d data to train", bag_data_cnt_);
    // set bagging data to tree learner
    tree_learner_[curr_class]->SetBaggingData(bag_data_indices_, bag_data_cnt_);
  }
}

void GBDT::UpdateScoreOutOfBag(const Tree* tree, const int curr_class) {
  // we need to predict out-of-bag socres of data for boosting
  if (out_of_bag_data_indices_ != nullptr) {
    train_score_updater_->
      AddScore(tree, out_of_bag_data_indices_, out_of_bag_data_cnt_, curr_class);
  }
}

bool GBDT::TrainOneIter(const score_t* gradient, const score_t* hessian, bool is_eval) {
    // boosting first
    if (gradient == nullptr || hessian == nullptr) {
      Boosting();
      gradient = gradients_;
      hessian = hessians_;
    }

    for (int curr_class = 0; curr_class < num_class_; ++curr_class){
      // bagging logic
      Bagging(iter_, curr_class);

      // train a new tree
      Tree * new_tree = tree_learner_[curr_class]->Train(gradient + curr_class * num_data_, hessian+ curr_class * num_data_);
      // if cannot learn a new tree, then stop
      if (new_tree->num_leaves() <= 1) {
        Log::Info("Stopped training because there are no more leafs that meet the split requirements.");
        return true;
      }

      // shrinkage by learning rate
      new_tree->Shrinkage(gbdt_config_->learning_rate);
      // update score
      UpdateScore(new_tree, curr_class);
      UpdateScoreOutOfBag(new_tree, curr_class);

      // add model
      models_.push_back(new_tree);
    }

  bool is_met_early_stopping = false;
  // print message for metric
  if (is_eval) {
    is_met_early_stopping = OutputMetric(iter_ + 1);
  }
  ++iter_;
  if (is_met_early_stopping) {
    Log::Info("Early stopping at iteration %d, the best iteration round is %d",
      iter_, iter_ - early_stopping_round_);
    // pop last early_stopping_round_ models
    for (int i = 0; i < early_stopping_round_ * num_class_; ++i) {
      delete models_.back();
      models_.pop_back();
    }
  }
  return is_met_early_stopping;

}

void GBDT::UpdateScore(const Tree* tree, const int curr_class) {
  // update training score
  train_score_updater_->AddScore(tree_learner_[curr_class], curr_class);
  // update validation score
  for (auto& score_updater : valid_score_updater_) {
    score_updater->AddScore(tree, curr_class);
  }
}

bool GBDT::OutputMetric(int iter) {
  bool ret = false;
  // print training metric
  if ((iter % gbdt_config_->output_freq) == 0) {
    for (auto& sub_metric : training_metrics_) {
      auto name = sub_metric->GetName();
      auto scores = sub_metric->Eval(train_score_updater_->score());
      for (size_t k = 0; k < name.size(); ++k) {
        Log::Info("Iteration: %d, %s : %f", iter, name[k].c_str(), scores[k]);
      }
    }
  }
  // print validation metric
  if ((iter % gbdt_config_->output_freq) == 0 || early_stopping_round_ > 0) {
    for (size_t i = 0; i < valid_metrics_.size(); ++i) {
      for (size_t j = 0; j < valid_metrics_[i].size(); ++j) {
        auto test_scores = valid_metrics_[i][j]->Eval(valid_score_updater_[i]->score());
        if ((iter % gbdt_config_->output_freq) == 0) {
          auto name = valid_metrics_[i][j]->GetName();
          for (size_t k = 0; k < name.size(); ++k) {
            Log::Info("Iteration: %d, %s : %f", iter, name[k].c_str(), test_scores[k]);
          }
        }
        if (!ret && early_stopping_round_ > 0) {
          auto cur_score = valid_metrics_[i][j]->factor_to_bigger_better() * test_scores.back();
          if (cur_score > best_score_[i][j]) {
            best_score_[i][j] = cur_score;
            best_iter_[i][j] = iter;
          } else {
            if (iter - best_iter_[i][j] >= early_stopping_round_) { ret = true; }
          }
        }
      }
    }
  }
  return ret;
}

/*! \brief Get eval result */
std::vector<double> GBDT::GetEvalAt(int data_idx) const {
  CHECK(data_idx >= 0 && data_idx <= static_cast<int>(valid_metrics_.size()));
  std::vector<double> ret;
  if (data_idx == 0) {
    for (auto& sub_metric : training_metrics_) {
      auto scores = sub_metric->Eval(train_score_updater_->score());
      for (auto score : scores) {
        ret.push_back(score);
      }
    }
  }
  else {
    auto used_idx = data_idx - 1;
    for (size_t j = 0; j < valid_metrics_[used_idx].size(); ++j) {
      auto test_scores = valid_metrics_[used_idx][j]->Eval(valid_score_updater_[used_idx]->score());
      for (auto score : test_scores) {
        ret.push_back(score);
      }
    }
  }
  return ret;
}

/*! \brief Get training scores result */
const score_t* GBDT::GetTrainingScore(data_size_t* out_len) {
  *out_len = train_score_updater_->num_data() * num_class_;
  return train_score_updater_->score();
}

void GBDT::GetPredictAt(int data_idx, score_t* out_result, data_size_t* out_len) const {
  CHECK(data_idx >= 0 && data_idx <= static_cast<int>(valid_metrics_.size()));
  std::vector<double> ret;

  const score_t* raw_scores = nullptr;
  data_size_t num_data = 0;
  if (data_idx == 0) {
    raw_scores = train_score_updater_->score();
    num_data = train_score_updater_->num_data();
  } else {
    auto used_idx = data_idx - 1;
    raw_scores = valid_score_updater_[used_idx]->score();
    num_data = valid_score_updater_[used_idx]->num_data();
  }
  *out_len = num_data * num_class_;

  if (num_class_ > 1) {
#pragma omp parallel for schedule(guided)
    for (data_size_t i = 0; i < num_data; ++i) {
      std::vector<double> tmp_result;
      for (int j = 0; j < num_class_; ++j) {
        tmp_result.push_back(raw_scores[j * num_data + i]);
      }
      Common::Softmax(&tmp_result);
      for (int j = 0; j < num_class_; ++j) {
        out_result[j * num_data + i] = static_cast<score_t>(tmp_result[i]);
      }
    }
  } else if(sigmoid_ > 0){
#pragma omp parallel for schedule(guided)
    for (data_size_t i = 0; i < num_data; ++i) {
      out_result[i] = static_cast<score_t>(1.0f / (1.0f + std::exp(-2.0f * sigmoid_ * raw_scores[i])));
    }
  } else {
#pragma omp parallel for schedule(guided)
    for (data_size_t i = 0; i < num_data; ++i) {
      out_result[i] = raw_scores[i];
    }
  }

}

void GBDT::Boosting() {
  if (object_function_ == nullptr) {
    Log::Fatal("No object function provided");
  }
  // objective function will calculate gradients and hessians
  int num_score = 0;
  object_function_->
    GetGradients(GetTrainingScore(&num_score), gradients_, hessians_);
}

void GBDT::SaveModelToFile(int num_used_model, bool is_finish, const char* filename) {

  // first time to this function, open file
  if (saved_model_size_ < 0) {
    model_output_file_.open(filename);
    // output model type
    model_output_file_ << Name() << std::endl;
    // output number of class
    model_output_file_ << "num_class=" << num_class_ << std::endl;
    // output label index
    model_output_file_ << "label_index=" << label_idx_ << std::endl;
    // output max_feature_idx
    model_output_file_ << "max_feature_idx=" << max_feature_idx_ << std::endl;
    // output sigmoid parameter
    model_output_file_ << "sigmoid=" << object_function_->GetSigmoid() << std::endl;
    model_output_file_ << std::endl;
    saved_model_size_ = 0;
  }
  // already saved
  if (!model_output_file_.is_open()) {
    return;
  }
  if (num_used_model == NO_LIMIT) {
    num_used_model = static_cast<int>(models_.size());
  } else {
    num_used_model = num_used_model * num_class_;
  }
  int rest = num_used_model - early_stopping_round_ * num_class_;
  // output tree models
  for (int i = saved_model_size_; i < rest; ++i) {
    model_output_file_ << "Tree=" << i << std::endl;
    model_output_file_ << models_[i]->ToString() << std::endl;
  }

  saved_model_size_ = Common::Max(saved_model_size_, rest);

  model_output_file_.flush();
  // training finished, can close file
  if (is_finish) {
    for (int i = saved_model_size_; i < num_used_model; ++i) {
      model_output_file_ << "Tree=" << i << std::endl;
      model_output_file_ << models_[i]->ToString() << std::endl;
    }
    model_output_file_ << std::endl << FeatureImportance() << std::endl;
    model_output_file_.close();
  }
}

void GBDT::LoadModelFromString(const std::string& model_str) {
  // use serialized string to restore this object
  models_.clear();
  std::vector<std::string> lines = Common::Split(model_str.c_str(), '\n');

  // get number of classes
  auto line = Common::FindFromLines(lines, "num_class=");
  if (line.size() > 0) {
    Common::Atoi(Common::Split(line.c_str(), '=')[1].c_str(), &num_class_);
  } else {
    Log::Fatal("Model file doesn't specify the number of classes");
    return;
  }
  // get index of label
  line = Common::FindFromLines(lines, "label_index=");
  if (line.size() > 0) {
    Common::Atoi(Common::Split(line.c_str(), '=')[1].c_str(), &label_idx_);
  } else {
    Log::Fatal("Model file doesn't specify the label index");
    return;
  }
  // get max_feature_idx first
  line = Common::FindFromLines(lines, "max_feature_idx=");
  if (line.size() > 0) {
    Common::Atoi(Common::Split(line.c_str(), '=')[1].c_str(), &max_feature_idx_);
  } else {
    Log::Fatal("Model file doesn't specify max_feature_idx");
    return;
  }
  // get sigmoid parameter
  line = Common::FindFromLines(lines, "sigmoid=");
  if (line.size() > 0) {
    Common::Atof(Common::Split(line.c_str(), '=')[1].c_str(), &sigmoid_);
  } else {
    sigmoid_ = -1.0f;
  }
  // get tree models
  size_t i = 0;
  while (i < lines.size()) {
    size_t find_pos = lines[i].find("Tree=");
    if (find_pos != std::string::npos) {
      ++i;
      int start = static_cast<int>(i);
      while (i < lines.size() && lines[i].find("Tree=") == std::string::npos) { ++i; }
      int end = static_cast<int>(i);
      std::string tree_str = Common::Join<std::string>(lines, start, end, '\n');
      models_.push_back(new Tree(tree_str));
    } else {
      ++i;
    }
  }
  Log::Info("Finished loading %d models", models_.size());
  num_used_model_ = static_cast<int>(models_.size()) / num_class_;
}

std::string GBDT::FeatureImportance() const {
  std::vector<size_t> feature_importances(max_feature_idx_ + 1, 0);
    for (size_t iter = 0; iter < models_.size(); ++iter) {
        for (int split_idx = 0; split_idx < models_[iter]->num_leaves() - 1; ++split_idx) {
            ++feature_importances[models_[iter]->split_feature_real(split_idx)];
        }
    }
    // store the importance first
    std::vector<std::pair<size_t, std::string>> pairs;
    for (size_t i = 0; i < feature_importances.size(); ++i) {
      if (feature_importances[i] > 0) {
        pairs.emplace_back(feature_importances[i], train_data_->feature_names()[i]);
      }
    }
    // sort the importance
    std::sort(pairs.begin(), pairs.end(),
      [](const std::pair<size_t, std::string>& lhs,
        const std::pair<size_t, std::string>& rhs) {
      return lhs.first > rhs.first;
    });
    std::stringstream str_buf;
    // write to model file
    str_buf << std::endl << "feature importances:" << std::endl;
    for (size_t i = 0; i < pairs.size(); ++i) {
      str_buf << pairs[i].second << "=" << std::to_string(pairs[i].first) << std::endl;
    }
    return str_buf.str();
}

std::vector<double> GBDT::PredictRaw(const double* value) const {
  std::vector<double> ret(num_class_, 0.0f);
  for (int i = 0; i < num_used_model_; ++i) {
    for (int j = 0; j < num_class_; ++j) {
      ret[j] += models_[i * num_class_ + j]->Predict(value);
    }
  }
  return ret;
}

std::vector<double> GBDT::Predict(const double* value) const {
  std::vector<double> ret(num_class_, 0.0f);
  for (int i = 0; i < num_used_model_; ++i) {
    for (int j = 0; j < num_class_; ++j) {
      ret[j] += models_[i * num_class_ + j]->Predict(value);
    }
  }
  // if need sigmoid transform
  if (sigmoid_ > 0 && num_class_ == 1) {
    ret[0] = 1.0f / (1.0f + std::exp(- 2.0f * sigmoid_ * ret[0]));
  } else if (num_class_ > 1) {
    Common::Softmax(&ret);
  }
  return ret;
}

std::vector<int> GBDT::PredictLeafIndex(const double* value) const {
  std::vector<int> ret;
  for (int i = 0; i < num_used_model_; ++i) {
    for (int j = 0; j < num_class_; ++j) {
      ret.push_back(models_[i * num_class_ + j]->PredictLeafIndex(value));
    }
  }
  return ret;
}

}  // namespace LightGBM
