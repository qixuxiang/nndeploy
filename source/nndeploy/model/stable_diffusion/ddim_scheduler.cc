
#include "nndeploy/model/stable_diffusion/ddim_scheduler.h"

#include "nndeploy/model/infer.h"
#include "nndeploy/model/stable_diffusion/scheduler.h"
#include "nndeploy/op/function.h"

namespace nndeploy {
namespace model {

TypeSchedulerRegister<TypeSchedulerCreator<DDIMScheduler>>
    g_ddim_scheduler_register(kSchedulerTypeDDIM);

DDIMScheduler::DDIMScheduler(const std::string &name,
                             SchedulerType scheduler_type, dag::Edge *input,
                             dag::Edge *output)
    : Scheduler(name, scheduler_type, input, output) {
  param_ = std::make_shared<SchedulerParam>();
}
DDIMScheduler::DDIMScheduler(const std::string &name,
                             SchedulerType scheduler_type,
                             std::initializer_list<dag::Edge *> inputs,
                             std::initializer_list<dag::Edge *> outputs)
    : Scheduler(name, scheduler_type, inputs, outputs) {
  param_ = std::make_shared<SchedulerParam>();
}
DDIMScheduler::DDIMScheduler(const std::string &name,
                             SchedulerType scheduler_type,
                             std::vector<dag::Edge *> &inputs,
                             std::vector<dag::Edge *> &outputs)
    : Scheduler(name, scheduler_type, inputs, outputs) {
  param_ = std::make_shared<SchedulerParam>();
}
DDIMScheduler::~DDIMScheduler() {}

base::Status DDIMScheduler::setTimesteps() {
  base::Status status = base::kStatusCodeOk;
  SchedulerParam *scheduler_param = (SchedulerParam *)(param_.get());
  int step_ratio = scheduler_param->num_train_timesteps_ /
                   scheduler_param->num_inference_steps_;
  timesteps_.clear();
  // creates integer timesteps by multiplying by ratio
  // casting to int to avoid issues when num_inference_step is power of 3
  timesteps_.resize(scheduler_param->num_inference_steps_);
  for (int i = 0; i < scheduler_param->num_inference_steps_; i++) {
    timesteps_[i] =
        (int64_t)((scheduler_param->num_inference_steps_ - 1 - i) * step_ratio +
                  scheduler_param->steps_offset_);
  }
  return status;
}

device::Tensor *DDIMScheduler::scaleModelInput(device::Tensor *sample,
                                               int index) {
  return sample;
}

float DDIMScheduler::getVariance(int64_t timesteps, int64_t prev_timestep) {
  float alpha_prod_t = alphas_cumprod_[timesteps];
  float alpha_prod_t_prev = (prev_timestep >= 0)
                                ? alphas_cumprod_[prev_timestep]
                                : final_alpha_cumprod_;
  float beta_prod_t = 1.0 - alpha_prod_t;
  float beta_prod_t_prev = 1.0 - alpha_prod_t_prev;

  float variance = (beta_prod_t_prev / beta_prod_t) *
                   (1.0 - alpha_prod_t / alpha_prod_t_prev);
  return variance;
}

base::Status DDIMScheduler::configure() {
  base::Status status = base::kStatusCodeOk;
  SchedulerParam *scheduler_param = (SchedulerParam *)(param_.get());
  variance_.resize(scheduler_param->num_inference_steps_, 0.0f);
  int64_t step_ratio = (int64_t)scheduler_param->num_train_timesteps_ /
                       (int64_t)scheduler_param->num_inference_steps_;
  for (int i = 0; i < scheduler_param->num_inference_steps_; i++) {
    int64_t prev_timestep = timesteps_[i] - step_ratio;
    variance_[i] = getVariance(timesteps_[i], prev_timestep);
  }
  return status;
}
/**
 * @brief
 *
 * @param sample
 * @param latents
 * @param index
 * @param timestep
 * @return base::Status
 * # See formulas (12) and (16) of DDIM paper
 * # https://arxiv.org/pdf/2010.02502.pdf
 * # Ideally, read DDIM paper in-detail understanding
 * # Notation (<variable name> -> <name in paper>
 * # - pred_noise_t -> e_theta(x_t, t)
 * # - pred_original_sample -> f_theta(x_t, t) or x_0
 * # - std_dev_t -> sigma_t
 * # - eta -> η
 * # - pred_sample_direction -> "direction pointing to x_t"
 * # - pred_prev_sample -> "x_t-1"
 */
base::Status DDIMScheduler::step(device::Tensor *model_output,
                                 device::Tensor *sample, int idx,
                                 std::vector<int64_t> &timestep, float eta,
                                 bool use_clipped_model_output,
                                 std::mt19937 &generator,
                                 device::Tensor *variance_noise) {
  base::Status status = base::kStatusCodeOk;
  SchedulerParam *scheduler_param = (SchedulerParam *)(param_.get());
  device::Device *host_device = device::getDefaultHostDevice();

  //
  int prev_idx = idx + 1;

  float alpha_prod_t = alphas_cumprod_[idx];
  device::TensorDesc alpha_prod_t_desc(base::dataTypeOf<float>(),
                                       base::kDataFormatN, {1});
  device::Tensor alpha_prod_t_tensor(host_device, alpha_prod_t_desc);
  alpha_prod_t_tensor.set(alpha_prod_t);
  float alpha_prod_t_sqrt = std::sqrtf(alpha_prod_t);
  device::Tensor alpha_prod_t_sqrt_tensor(host_device, alpha_prod_t_desc);
  alpha_prod_t_sqrt_tensor.set(alpha_prod_t_sqrt);

  float alpha_prod_t_prev = (prev_idx < scheduler_param->num_train_timesteps_)
                                ? alphas_cumprod_[prev_idx]
                                : final_alpha_cumprod_;
  device::TensorDesc alpha_prod_t_prev_desc(base::dataTypeOf<float>(),
                                            base::kDataFormatN, {1});
  device::Tensor alpha_prod_t_prev_tensor(host_device, alpha_prod_t_prev_desc);
  alpha_prod_t_prev_tensor.set(alpha_prod_t_prev);

  float beta_prod_t = 1.0 - alpha_prod_t;
  device::TensorDesc beta_prod_t_desc(base::dataTypeOf<float>(),
                                      base::kDataFormatN, {1});
  device::Tensor beta_prod_t_tensor(host_device, beta_prod_t_desc);
  beta_prod_t_tensor.set(beta_prod_t);
  float beta_prod_t_sqrt = std::sqrtf(beta_prod_t);
  device::Tensor beta_prod_t_sqrt_tensor(host_device, beta_prod_t_desc);
  beta_prod_t_sqrt_tensor.set(beta_prod_t_sqrt);

  // 3. compute predicted original sample from predicted noise also called
  // "predicted x_0" of formula(12) from https: //arxiv.org/pdf/2010.02502.pdf
  device::Tensor pred_original_sample(model_output->getDevice(),
                                      model_output->getDesc());
  if (scheduler_param->prediction_type_ == "epsilon") {
    device::Tensor tmp1(model_output->getDevice(), model_output->getDesc());
    op::mul(&beta_prod_t_sqrt_tensor, model_output, &tmp1);
    device::Tensor tmp2(model_output->getDevice(), model_output->getDesc());
    op::sub(sample, &tmp1, &tmp2);
    op::div(&tmp2, &tmp1, &alpha_prod_t_sqrt_tensor);
  } else if (scheduler_param->prediction_type_ == "sample") {
    model_output->copyTo(&pred_original_sample);
  } else if (scheduler_param->prediction_type_ == "v_prediction") {
    device::Tensor sample_tmp(sample->getDevice(), sample->getDesc());
    op::mul(&alpha_prod_t_sqrt_tensor, sample, &sample_tmp);
    device::Tensor model_output_tmp(model_output->getDevice(),
                                    model_output->getDesc());
    op::mul(&beta_prod_t_sqrt_tensor, model_output, &model_output_tmp);
    op::sub(&sample_tmp, &model_output_tmp, &pred_original_sample);

    op::mul(&beta_prod_t_sqrt_tensor, sample, &sample_tmp);
    op::mul(&alpha_prod_t_sqrt_tensor, model_output, &model_output_tmp);
    op::add(&sample_tmp, &model_output_tmp, model_output);
  } else {
    NNDEPLOY_LOGE("Invalid prediction type!\n");
    return base::kStatusCodeErrorInvalidValue;
  }

  // # 4. Clip "predicted x_0"
  if (scheduler_param->clip_sample_) {
    op::clamp(&pred_original_sample, -1.0f, 1.0f, &pred_original_sample);
  }

  // # 5. compute variance : "sigma_t(η)"->see formula(16)
  // σ_t = sqrt((1 − α_t−1) / (1 − α_t)) * sqrt(1 − α_t / α_t−1)
  float variance = variance_[idx];
  float std_dev_t = eta * sqrtf(variance);

  if (use_clipped_model_output) {
    // the model_output is always re-derived from the clipped x_0 in Glide
    device::Tensor pred_original_sample_tmp(pred_original_sample.getDevice(),
                                            pred_original_sample.getDesc());
    op::mul(&alpha_prod_t_sqrt_tensor, &pred_original_sample,
            &pred_original_sample_tmp);
    device::Tensor sample_tmp(sample->getDevice(), sample->getDesc());
    op::sub(sample, &pred_original_sample_tmp, &sample_tmp);
    op::div(&sample_tmp, &beta_prod_t_sqrt_tensor, model_output);
  }

  // # 6. compute "direction pointing to x_t" of formula (12) from
  // https://arxiv.org/pdf/2010.02502.pdf
  float coff_model_output =
      std::sqrtf(1 - alpha_prod_t_prev - std_dev_t * std_dev_t);
  device::Tensor coff_model_output_tensor(host_device, beta_prod_t_desc);
  coff_model_output_tensor.set(coff_model_output);
  device::Tensor pred_sample_direction(model_output->getDevice(),
                                       model_output->getDesc());
  op::mul(&coff_model_output_tensor, model_output, &pred_sample_direction);

  // # 7. compute x_t without "random noise" of formula(12) from https:
  // //arxiv.org/pdf/2010.02502.pdf
  float alpha_prod_t_prev_sqrt = std::sqrtf(alpha_prod_t_prev);
  device::Tensor pred_original_sample_temp;
  pred_original_sample_temp = alpha_prod_t_prev_sqrt * pred_original_sample;
  device::Tensor prev_sample;
  prev_sample = pred_original_sample_temp + pred_sample_direction;

  return status;
}

base::Status DDIMScheduler::addNoise(device::Tensor *init_latents,
                                     device::Tensor *noise, int idx,
                                     int latent_timestep) {
  base::Status status = base::kStatusCodeOk;
  device::Device *host_device = device::getDefaultHostDevice();

  float sqrt_alpha_prod = std::sqrtf(alphas_cumprod_[idx]);
  device::TensorDesc sqrt_alpha_prod_desc(base::dataTypeOf<float>(),
                                          base::kDataFormatN, {1});
  device::Tensor sqrt_alpha_prod_tensor(host_device, sqrt_alpha_prod_desc);
  sqrt_alpha_prod_tensor.set(sqrt_alpha_prod);
  op::mul(&sqrt_alpha_prod_tensor, init_latents, init_latents);

  float sqrt_one_minus_alpha_prod = std::sqrtf(1.0f - alphas_cumprod_[idx]);
  device::TensorDesc sqrt_one_minus_alpha_prod_desc(base::dataTypeOf<float>(),
                                                    base::kDataFormatN, {1});
  device::Tensor sqrt_one_minus_alpha_prod_tensor(host_device,
                                                  sqrt_alpha_prod_desc);
  sqrt_one_minus_alpha_prod_tensor.set(sqrt_one_minus_alpha_prod);
  op::mul(&sqrt_one_minus_alpha_prod_tensor, noise, noise);

  op::add(init_latents, noise, init_latents);

  return status;
}

base::Status DDIMScheduler::init() {
  base::Status status = base::kStatusCodeOk;

  // # this schedule is very specific to the latent diffusion model.
  // ## 计算betas，它们是方差的平方根，从beta_start的平方根到beta_end的平方根
  SchedulerParam *scheduler_param = (SchedulerParam *)(param_.get());
  std::vector<float> betas;
  betas.resize(scheduler_param->num_train_timesteps_);
  customLinspace(std::sqrtf(scheduler_param->beta_start_),
                 std::sqrtf(scheduler_param->beta_end_),
                 scheduler_param->num_train_timesteps_, betas);
  // ## 计算alphas，它们是1减去beta的平方
  std::vector<float> alphas(scheduler_param->num_train_timesteps_, 0.0f);
  for (int i = 0; i < scheduler_param->num_train_timesteps_; i++) {
    alphas[i] = 1 - betas[i] * betas[i];
  }
  // ## alphas_cumprod_
  alphas_cumprod_.resize(scheduler_param->num_train_timesteps_, 0.0f);
  alphas_cumprod_[0] = alphas[0];
  for (int i = 1; i < scheduler_param->num_train_timesteps_; i++) {
    alphas_cumprod_[i] = alphas_cumprod_[i - 1] * alphas[i];
  }
  // # standard deviation of the initial noise distribution
  init_noise_sigma_ = 1.0f;

  // # At every step in ddim, we are looking into the previous alphas_cumprod
  // For the final step, there is no previous alphas_cumprod because we are
  // already at 0 `set_alpha_to_one` decides whether we set this parameter
  // simply to one or whether we use the final alpha of the "non-previous"
  // one.
  if (scheduler_param->set_alpha_to_one_) {
    final_alpha_cumprod_ = 1.0;
  } else {
    final_alpha_cumprod_ = alphas_cumprod_.front();
  }

  // timesteps_
  timesteps_.resize(scheduler_param->num_train_timesteps_, 0);
  for (int i = 0; i < scheduler_param->num_train_timesteps_; i++) {
    timesteps_[i] = scheduler_param->num_train_timesteps_ - 1 - i;
  }

  status = this->setTimesteps();
  NNDEPLOY_RETURN_ON_NEQ(status, base::kStatusCodeOk, "setTimesteps failed!");

  status = this->configure();
  NNDEPLOY_RETURN_ON_NEQ(status, base::kStatusCodeOk, "configure failed!");

  status = Loop::init();
  NNDEPLOY_RETURN_ON_NEQ(status, base::kStatusCodeOk, "Loop::init failed!");

  return status;
}
base::Status DDIMScheduler::deinit() {
  base::Status status = base::kStatusCodeOk;
  return status;
}

int DDIMScheduler::loops() {
  SchedulerParam *scheduler_param = (SchedulerParam *)(param_.get());
  return scheduler_param->num_inference_steps_;
}

base::Status DDIMScheduler::run() {
  base::Status status = base::kStatusCodeOk;

  setRunningFlag(true);

  SchedulerParam *scheduler_param = (SchedulerParam *)(param_.get());

  int size = loops();
  if (size < 1) {
    NNDEPLOY_LOGE("loops size is invalid!\n");
    return base::kStatusCodeErrorInvalidValue;
  }
  device::Device *host_device = device::getDefaultHostDevice();

  // encoder_hidden_states
  dag::Edge *encoder_hidden_states = this->getEdge("encoder_hidden_states");
  device::Tensor *encoder_hidden_states_tensor =
      encoder_hidden_states->getTensor(this);
  int index = encoder_hidden_states->getIndex(this);
  int batch_size = encoder_hidden_states_tensor->getBatch();

  // build sample
  dag::Edge *sample = this->getEdge("sample");
  device::TensorDesc sample_desc;
  sample_desc.data_type_ = base::dataTypeOf<float>();
  sample_desc.data_format_ = base::kDataFormatNCHW;
  sample_desc.shape_.emplace_back(batch_size * 2);
  sample_desc.shape_.emplace_back(scheduler_param->unet_channels_);
  int latent_height = scheduler_param->image_height_ / 8;
  sample_desc.shape_.emplace_back(latent_height);
  int latent_width = scheduler_param->image_width_ / 8;
  sample_desc.shape_.emplace_back(latent_width);
  device::Tensor *sample_tensor =
      sample->create(host_device, sample_desc, index);

  // build timestep
  dag::Edge *timestep = this->getEdge("timestep");
  device::TensorDesc timestep_desc;
  timestep_desc.data_type_ = base::dataTypeOf<float>();
  timestep_desc.data_format_ = base::kDataFormatNC;
  timestep_desc.shape_.emplace_back(batch_size);
  timestep_desc.shape_.emplace_back(1);
  device::Tensor *timestep_tensor =
      timestep->create(host_device, timestep_desc, index);

  // build latent
  device::TensorDesc latent_desc;
  latent_desc.data_type_ = base::dataTypeOf<float>();
  latent_desc.data_format_ = base::kDataFormatNCHW;
  latent_desc.shape_.emplace_back(batch_size);
  latent_desc.shape_.emplace_back(scheduler_param->unet_channels_);
  int latent_height = scheduler_param->image_height_ / 8;
  latent_desc.shape_.emplace_back(latent_height);
  int latent_width = scheduler_param->image_width_ / 8;
  latent_desc.shape_.emplace_back(latent_width);
  device::Tensor *latent =
      outputs_[0]->create(host_device, latent_desc, inputs_[0]->getIndex(this));
  std::mt19937 generator;
  initializeLatents(generator, init_noise_sigma_, latent);

  for (int i = 0; i < size; i++) {
    // op::concat({latent, latent}, 0, sample);
    // status = executor_->run();
    // NNDEPLOY_RETURN_ON_NEQ(status, base::kStatusCodeOk, "executor run
    // failed!");
    // op::split(sample, 0, noise_pred_uncond_, noise_pred_text_)
    // step(noise_pred_uncond_, sample, i, timestep, eta, false, generator,
    //      nullptr);
  }

  // float scale = 1.0f / 0.18215f;
  // op::mul(latents, scale, latents);

  setRunningFlag(false);
  return status;
}

}  // namespace model
}  // namespace nndeploy
