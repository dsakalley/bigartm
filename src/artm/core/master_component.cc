// Copyright 2014, Additive Regularization of Topic Models.

#include "artm/core/master_component.h"

#include <algorithm>
#include <fstream>  // NOLINT
#include <vector>
#include <set>
#include <sstream>

#include "glog/logging.h"

#include "artm/regularizer_interface.h"
#include "artm/score_calculator_interface.h"

#include "artm/core/exceptions.h"
#include "artm/core/helpers.h"
#include "artm/core/data_loader.h"
#include "artm/core/batch_manager.h"
#include "artm/core/cache_manager.h"
#include "artm/core/instance.h"
#include "artm/core/processor.h"
#include "artm/core/topic_model.h"
#include "artm/core/merger.h"

namespace artm {
namespace core {

MasterComponent::MasterComponent(int id, const MasterComponentConfig& config)
    : is_configured_(false),
      master_id_(id),
      config_(std::make_shared<MasterComponentConfig>(config)),
      instance_(nullptr) {
  LOG(INFO) << "Creating MasterComponent (id=" << master_id_ << ")...";
  Reconfigure(config);
}

MasterComponent::~MasterComponent() {
  LOG(INFO) << "Disposing MasterComponent (id=" << master_id_ << ")...";
}

int MasterComponent::id() const {
  return master_id_;
}

void MasterComponent::CreateOrReconfigureModel(const ModelConfig& config) {
  if ((config.class_weight_size() != 0 || config.class_id_size() != 0) && !config.use_sparse_bow()) {
    std::stringstream ss;
    ss << "You have configured use_sparse_bow=false. "
       << "Fields ModelConfig.class_id and ModelConfig.class_weight not supported in this mode.";
    BOOST_THROW_EXCEPTION(InvalidOperation(ss.str()));
  }

  LOG(INFO) << "Merger::CreateOrReconfigureModel() with " << Helpers::Describe(config);
  instance_->CreateOrReconfigureModel(config);
}

void MasterComponent::DisposeModel(ModelName model_name) {
  instance_->DisposeModel(model_name);
}

void MasterComponent::CreateOrReconfigureRegularizer(const RegularizerConfig& config) {
  instance_->CreateOrReconfigureRegularizer(config);
}

void MasterComponent::DisposeRegularizer(const std::string& name) {
  instance_->DisposeRegularizer(name);
}

void MasterComponent::CreateOrReconfigureDictionary(const DictionaryConfig& config) {
  instance_->CreateOrReconfigureDictionary(config);
}

void MasterComponent::DisposeDictionary(const std::string& name) {
  instance_->DisposeDictionary(name);
}

void MasterComponent::SynchronizeModel(const SynchronizeModelArgs& args) {
  instance_->merger()->ForceSynchronizeModel(args);
}

void MasterComponent::ExportModel(const ExportModelArgs& args) {
  if (boost::filesystem::exists(args.file_name()))
    BOOST_THROW_EXCEPTION(DiskWriteException("File already exists: " + args.file_name()));

  std::ofstream fout(args.file_name(), std::ofstream::binary);
  if (!fout.is_open())
    BOOST_THROW_EXCEPTION(DiskReadException("Unable to create file " + args.file_name()));

  std::shared_ptr<const ::artm::core::TopicModel> topic_model =
    instance_->merger()->GetLatestTopicModel(args.model_name());
  if (topic_model == nullptr)
    BOOST_THROW_EXCEPTION(InvalidOperation("Model " + args.model_name() + " does not exist"));

  LOG(INFO) << "Exporting model " << args.model_name() << " to " << args.file_name();

  const int token_size = topic_model->token_size();
  int tokens_per_chunk = std::min<int>(token_size, 100 * 1024 * 1024 / topic_model->topic_size());

  ::artm::GetTopicModelArgs get_topic_model_args;
  get_topic_model_args.set_model_name(args.model_name());
  get_topic_model_args.set_request_type(::artm::GetTopicModelArgs_RequestType_Nwt);
  get_topic_model_args.set_use_sparse_format(true);
  get_topic_model_args.mutable_token()->Reserve(tokens_per_chunk);
  get_topic_model_args.mutable_class_id()->Reserve(tokens_per_chunk);
  for (int token_id = 0; token_id < token_size; ++token_id) {
    Token token = topic_model->token(token_id);
    get_topic_model_args.add_token(token.keyword);
    get_topic_model_args.add_class_id(token.class_id);

    if (((token_id + 1) == token_size) || (get_topic_model_args.token_size() >= tokens_per_chunk)) {
      ::artm::TopicModel external_topic_model;
      topic_model->RetrieveExternalTopicModel(get_topic_model_args, &external_topic_model);
      std::string str = external_topic_model.SerializeAsString();
      fout << str.size();
      fout << str;
      get_topic_model_args.clear_class_id();
      get_topic_model_args.clear_token();
    }
  }

  fout.close();
  LOG(INFO) << "Export completed, token_size = " << topic_model->token_size()
            << ", topic_size = " << topic_model->topic_size();
}

void MasterComponent::ImportModel(const ImportModelArgs& args) {
  std::ifstream fin(args.file_name(), std::ifstream::binary);
  if (!fin.is_open())
    BOOST_THROW_EXCEPTION(DiskReadException("Unable to open file " + args.file_name()));

  LOG(INFO) << "Importing model " << args.model_name() << " from " << args.file_name();

  while (!fin.eof()) {
    int length;
    fin >> length;
    if (fin.eof())
      break;

    if (length <= 0)
      BOOST_THROW_EXCEPTION(CorruptedMessageException("Unable to read from " + args.file_name()));

    std::string buffer(length, '\0');
    fin.read(&buffer[0], length);
    ::artm::TopicModel topic_model;
    if (!topic_model.ParseFromArray(buffer.c_str(), length))
      BOOST_THROW_EXCEPTION(CorruptedMessageException("Unable to read from " + args.file_name()));

    topic_model.set_name(args.model_name());
    OverwriteTopicModel(topic_model);
  }

  fin.close();
  WaitIdle(WaitIdleArgs());

  SynchronizeModelArgs sync_args;
  sync_args.set_model_name(args.model_name());
  sync_args.set_apply_weight(1.0f);
  sync_args.set_decay_weight(0.0f);
  sync_args.set_invoke_regularizers(true);
  SynchronizeModel(sync_args);

  std::shared_ptr<const ::artm::core::TopicModel> topic_model =
    instance_->merger()->GetLatestTopicModel(args.model_name());
  if (topic_model == nullptr) {
    LOG(ERROR) << "Unable to find " << args.model_name() << " after import";
    return;
  }

  LOG(INFO) << "Import completed, token_size = " << topic_model->token_size()
            << ", topic_size = " << topic_model->topic_size();
}

void MasterComponent::InitializeModel(const InitializeModelArgs& args) {
  instance_->merger()->InitializeModel(args);
}

void MasterComponent::Reconfigure(const MasterComponentConfig& user_config) {
  LOG(INFO) << "Merger::CreateOrReconfigureModel() with " << Helpers::Describe(user_config);
  ValidateConfig(user_config);

  MasterComponentConfig config(user_config);  // make a copy
  if (!config.has_processor_queue_max_size()) {
    // The default setting for processor queue max size is to use the number of processors.
    config.set_processor_queue_max_size(config.processors_count());
  }

  config_.set(std::make_shared<MasterComponentConfig>(config));

  if (!is_configured_) {
    // First configuration
    instance_.reset(new Instance(config));
    is_configured_ = true;
  } else {
    instance_->Reconfigure(config);
  }
}

bool MasterComponent::RequestTopicModel(const ::artm::GetTopicModelArgs& get_model_args,
                                        ::artm::TopicModel* topic_model) {
  return instance_->merger()->RetrieveExternalTopicModel(get_model_args, topic_model);
}

void MasterComponent::RequestRegularizerState(RegularizerName regularizer_name,
                                              ::artm::RegularizerInternalState* regularizer_state) {
  instance_->merger()->RequestRegularizerState(regularizer_name, regularizer_state);
}

bool MasterComponent::RequestScore(const GetScoreValueArgs& get_score_args,
                                   ScoreData* score_data) {
  if (!get_score_args.has_batch()) {
    return instance_->merger()->RequestScore(get_score_args, score_data);
  }

  if (instance_->processor_size() == 0)
    BOOST_THROW_EXCEPTION(InternalError("No processors exist in the master component"));
  instance_->processor(0)->FindThetaMatrix(
    get_score_args.batch(), GetThetaMatrixArgs(), nullptr, get_score_args, score_data);
  return true;
}

void MasterComponent::OverwriteTopicModel(const ::artm::TopicModel& topic_model) {
  instance_->merger()->OverwriteTopicModel(topic_model);
}

bool MasterComponent::RequestThetaMatrix(const GetThetaMatrixArgs& get_theta_args,
                                         ::artm::ThetaMatrix* theta_matrix) {
  if (!get_theta_args.has_batch()) {
    return instance_->cache_manager()->RequestThetaMatrix(get_theta_args, theta_matrix);
  } else {
    if (instance_->processor_size() == 0)
      BOOST_THROW_EXCEPTION(InternalError("No processors exist in the master component"));
    instance_->processor(0)->FindThetaMatrix(
      get_theta_args.batch(), get_theta_args, theta_matrix, GetScoreValueArgs(), nullptr);
    return true;
  }
}

bool MasterComponent::WaitIdle(const WaitIdleArgs& args) {
  int timeout = args.timeout_milliseconds();
  LOG_IF(WARNING, timeout == 0) << "WaitIdleArgs.timeout_milliseconds == 0";
  WaitIdleArgs new_args;
  new_args.CopyFrom(args);
  auto time_start = boost::posix_time::microsec_clock::local_time();

  bool retval = instance_->data_loader()->WaitIdle(args);
  if (!retval) return false;

  auto time_end = boost::posix_time::microsec_clock::local_time();
  if (timeout != -1) {
    timeout -= (time_end - time_start).total_milliseconds();
    new_args.set_timeout_milliseconds(timeout);
  }

  return instance_->merger()->WaitIdle(new_args);
}

void MasterComponent::InvokeIteration(const InvokeIterationArgs& args) {
  if (args.reset_scores())
    instance_->merger()->ForceResetScores(ModelName());

  instance_->data_loader()->InvokeIteration(args);
}

bool MasterComponent::AddBatch(const AddBatchArgs& args) {
  int timeout = args.timeout_milliseconds();
  LOG_IF(WARNING, timeout == 0) << "AddBatchArgs.timeout_milliseconds == 0";
  if (args.reset_scores())
    instance_->merger()->ForceResetScores(ModelName());

  return instance_->data_loader()->AddBatch(args);
}

void MasterComponent::ValidateConfig(const MasterComponentConfig& config) {
  if (is_configured_) {
    std::shared_ptr<MasterComponentConfig> current_config = config_.get();
    if (current_config->disk_path() != config.disk_path()) {
      std::string message = "Changing disk_path is not supported.";
      BOOST_THROW_EXCEPTION(InvalidOperation(message));
    }
  }
}

}  // namespace core
}  // namespace artm
