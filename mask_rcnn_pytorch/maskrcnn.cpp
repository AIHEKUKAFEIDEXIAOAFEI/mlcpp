#include "maskrcnn.h"
#include "anchors.h"
#include "debug.h"
#include "detectionlayer.h"
#include "detectiontargetlayer.h"
#include "proposallayer.h"
#include "resnet.h"
#include "stateloader.h"

#include <cmath>
#include <experimental/filesystem>
#include <regex>

namespace fs = std::experimental::filesystem;

MaskRCNNImpl::MaskRCNNImpl(std::string model_dir,
                           std::shared_ptr<Config const> config)
    : model_dir_(model_dir), config_(config) {
  Build();
  InitializeWeights();
}

/* Runs the detection pipeline.
 * images: List of images, potentially of different sizes.
 * Returns a list of dicts, one dict per image. The dict contains:
 * rois: [N, (y1, x1, y2, x2)] detection bounding boxes
 * class_ids: [N] int class IDs
 * scores: [N] float probability scores for the class IDs
 * masks: [H, W, N] instance binary masks
 */
std::tuple<at::Tensor, at::Tensor> MaskRCNNImpl::Detect(
    at::Tensor images,
    const std::vector<ImageMeta>& image_metas) {
  // Run object detection
  auto [detections, mrcnn_mask] = PredictInference(images, image_metas);

  detections = detections.cpu();
  mrcnn_mask = mrcnn_mask.permute({0, 1, 3, 4, 2}).cpu();

  return {detections, mrcnn_mask};
}

void MaskRCNNImpl::Train(std::unique_ptr<CocoDataset> train_dataset,
                         std::unique_ptr<CocoDataset> val_dataset,
                         double learning_rate,
                         uint32_t epochs,
                         std::string layers_regex) {
  // Pre-defined layer regular expressions
  // clang-format off
  std::map<std::string, std::string> layers_regex_map = {
           // all layers but the backbone
           {"heads", "(fpn.P5\\_.*)|(fpn.P4\\_.*)|(fpn.P3\\_.*)|(fpn.P2\\_.*)|(rpn.*)|(classifier.*)|(mask.*)"},
           // From a specific Resnet stage and up
           {"3+", "(fpn.C3.*)|(fpn.C4.*)|(fpn.C5.*)|(fpn.P5\\_.*)|(fpn.P4\\_.*)|(fpn.P3\\_.*)|(fpn.P2\\_.*)|(rpn.*)|(classifier.*)|(mask.*)"},
           {"4+", "(fpn.C4.*)|(fpn.C5.*)|(fpn.P5\\_.*)|(fpn.P4\\_.*)|(fpn.P3\\_.*)|(fpn.P2\\_.*)|(rpn.*)|(classifier.*)|(mask.*)"},
           {"5+", "(fpn.C5.*)|(fpn.P5\\_.*)|(fpn.P4\\_.*)|(fpn.P3\\_.*)|(fpn.P2\\_.*)|(rpn.*)|(classifier.*)|(mask.*)"},
           // All layers
           {"all", ".*"}};
  // clang-format on

  auto layer_regex_i = layers_regex_map.find(layers_regex);
  if (layer_regex_i != layers_regex_map.end())
    layers_regex = layer_regex_i->second;
  SetTrainableLayers(layers_regex);

  auto train_loader = torch::data::make_data_loader(
      *train_dataset,
      torch::data::DataLoaderOptions().batch_size(1).workers(
          4));  // random sampler is default

  auto val_loader = torch::data::make_data_loader(
      *val_dataset,
      torch::data::DataLoaderOptions().batch_size(1).workers(
          4));  // random sampler is default

  // Optimizer object
  // Add L2 Regularization
  // Skip gamma and beta weights of batch normalization layers.
  std::vector<torch::Tensor> trainable_params_no_bn;
  std::vector<torch::Tensor> trainable_params_bn;
  auto params = named_parameters(true /*recurse*/);
  for (auto& param : params) {
    auto& name = param.key();
    bool requires_grad = param.value().requires_grad();
    bool is_bn = name.find("bn") != std::string::npos;
    if (requires_grad && !is_bn) {
      trainable_params_no_bn.push_back(param.value());
    } else if (requires_grad && is_bn) {
      trainable_params_bn.push_back(param.value());
    }
  }
  torch::optim::SGD optim_no_bn(trainable_params_no_bn,
                                torch::optim::SGDOptions(learning_rate)
                                    .momentum(config_->learning_momentum)
                                    .weight_decay(config_->weight_decay));

  torch::optim::SGD optim_bn(trainable_params_bn,
                             torch::optim::SGDOptions(learning_rate)
                                 .momentum(config_->learning_momentum));

  for (uint32_t epoch = 0; epoch < epochs; ++epoch) {
    // Training
    auto [loss, loss_rpn_class, loss_rpn_bbox, loss_mrcnn_class,
          loss_mrcnn_bbox, loss_mrcnn_mask] =
        TrainEpoch(*train_loader, optim_no_bn, optim_bn,
                   config_->steps_per_epoch);

    // Validation
    // TODO: Implement validation
    //    auto [val_loss, val_loss_rpn_class, val_loss_rpn_bbox,
    //    val_loss_mrcnn_class,
    //          val_loss_mrcnn_bbox, val_loss_mrcnn_mask] =
    //        ValidEpoch(val_loader, config_->validation_steps);

    // Show statistics
    // TODO: Implement training statistics

    SaveStateDict(*this, GetCheckpointPath(epoch));
  }
}

std::tuple<float, float, float, float, float, float> MaskRCNNImpl::TrainEpoch(
    torch::data::DataLoader<CocoDataset, torch::data::samplers::RandomSampler>&
        datagenerator,
    torch::optim::SGD& optimizer,
    torch::optim::SGD& optimizer_bn,
    uint32_t steps) {
  uint32_t batch_count = 0;
  float loss_sum = 0;
  float loss_rpn_class_sum = 0;
  float loss_rpn_bbox_sum = 0;
  float loss_mrcnn_class_sum = 0;
  float loss_mrcnn_bbox_sum = 0;
  float loss_mrcnn_mask_sum = 0;
  uint32_t step = 0;

  optimizer.zero_grad();
  optimizer_bn.zero_grad();

  for (auto input : datagenerator) {
    ++batch_count;

    assert(input.size() == 1);

    // Wrap input in variables
    auto images = input[0].data.image.unsqueeze(0);  // add batch size dimention
    auto rpn_match = input[0].target.rpn_match;
    auto rpn_bbox = input[0].target.rpn_bbox;
    auto gt_class_ids = input[0].target.gt_class_ids;
    auto gt_boxes = input[0].target.gt_boxes;
    auto gt_masks = input[0].target.gt_masks;

    // To GPU
    if (config_->gpu_count > 0) {
      images = images.cuda();
      rpn_match = rpn_match.cuda();
      rpn_bbox = rpn_bbox.cuda();
      gt_class_ids = gt_class_ids.cuda();
      gt_boxes = gt_boxes.cuda();
      gt_masks = gt_masks.cuda();
    }

    // Run object detection
    auto [rpn_class_logits, rpn_pred_bbox, target_class_ids, mrcnn_class_logits,
          target_deltas, mrcnn_bbox, target_mask, mrcnn_mask] =
        PredictTraining(images, gt_class_ids, gt_boxes, gt_masks);
  }

  return {loss_sum,
          loss_rpn_class_sum,
          loss_rpn_bbox_sum,
          loss_mrcnn_class_sum,
          loss_mrcnn_bbox_sum,
          loss_mrcnn_mask_sum};
}

std::tuple<std::vector<at::Tensor>, at::Tensor, at::Tensor, at::Tensor>
MaskRCNNImpl::PredictRPN(at::Tensor images, uint32_t proposal_count) {
  // Feature extraction
  auto [p2_out, p3_out, p4_out, p5_out, p6_out] = fpn_->forward(images);

  // Note that P6 is used in RPN, but not in the classifier heads.
  std::vector<at::Tensor> rpn_feature_maps = {p2_out, p3_out, p4_out, p5_out,
                                              p6_out};
  std::vector<at::Tensor> mrcnn_feature_maps = {p2_out, p3_out, p4_out, p5_out};

  // Loop through pyramid layers
  std::vector<at::Tensor> rpn_class_logits;
  std::vector<at::Tensor> rpn_class;
  std::vector<at::Tensor> rpn_bbox;
  for (auto p : rpn_feature_maps) {
    auto [class_logits, probs, bbox] = rpn_->forward(p);
    rpn_class_logits.push_back(class_logits);
    rpn_class.push_back(probs);
    rpn_bbox.push_back(bbox);
  }

  // Generate proposals
  // Proposals are [batch, N, (y1, x1, y2, x2)] in normalized coordinates
  // and zero padded.
  auto scores = torch::cat(rpn_class, 1);
  auto deltas = torch::cat(rpn_bbox, 1);
  auto rpn_rois = ProposalLayer({scores, deltas}, proposal_count,
                                config_->rpn_nms_threshold, anchors_, *config_);

  auto class_logits = torch::cat(rpn_class_logits, 1);
  return {mrcnn_feature_maps, rpn_rois, class_logits, deltas};
}

std::tuple<at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor,
           at::Tensor>
MaskRCNNImpl::PredictTraining(at::Tensor images,
                              at::Tensor gt_class_ids,
                              at::Tensor gt_boxes,
                              at::Tensor gt_masks) {
  train();
  // Set batchnorm always in eval mode during training
  auto set_bn_eval = [](const std::string& name, Module& m) {
    if (name.find("BatchNorm") != std::string::npos) {
      m.eval();
    }
  };
  apply(set_bn_eval);

  auto [mrcnn_feature_maps, rpn_rois, rpn_class_logits, rpn_bbox] =
      PredictRPN(images, config_->post_nms_rois_inference);

  // Normalize coordinates
  auto h = static_cast<float>(config_->image_shape[0]);
  auto w = static_cast<float>(config_->image_shape[1]);
  auto scale =
      torch::tensor({h, w, h, w}, at::dtype(at::kFloat).requires_grad(false));
  if (config_->gpu_count > 0)
    scale = scale.cuda();
  gt_boxes = gt_boxes / scale;

  // Generate detection targets
  // Subsamples proposals and generates target outputs for training
  // Note that proposal class IDs, gt_boxes, and gt_masks are zero
  // padded. Equally, returned rois and targets are zero padded.
  auto [rois, target_class_ids, target_deltas, target_mask] =
      DetectionTargetLayer(*config_, rpn_rois, gt_class_ids, gt_boxes,
                           gt_masks);

  auto mrcnn_class_logits = torch::empty({}, at::dtype(at::kFloat));
  auto mrcnn_class = torch::empty({}, at::dtype(at::kInt));
  auto mrcnn_bbox = torch::empty({}, at::dtype(at::kFloat));
  auto mrcnn_mask = torch::empty({}, at::dtype(at::kFloat));
  if (config_->gpu_count > 0) {
    mrcnn_class_logits = mrcnn_class_logits.cuda();
    mrcnn_class = mrcnn_class.cuda();
    mrcnn_bbox = mrcnn_bbox.cuda();
    mrcnn_mask = mrcnn_mask.cuda();
  }

  if (!rois.sizes().empty()) {
    // Network Heads
    // Proposal classifier and BBox regressor heads
    std::tie(mrcnn_class_logits, mrcnn_class, mrcnn_bbox) =
        classifier_->forward(mrcnn_feature_maps, rois);

    // Create masks for detections
    mrcnn_mask = mask_->forward(mrcnn_feature_maps, rois);
  }

  return {rpn_class_logits, rpn_bbox,   target_class_ids, mrcnn_class_logits,
          target_deltas,    mrcnn_bbox, target_mask,      mrcnn_mask};
}

std::tuple<at::Tensor, at::Tensor> MaskRCNNImpl::PredictInference(
    at::Tensor images,
    const std::vector<ImageMeta>& image_metas) {
  eval();

  auto [mrcnn_feature_maps, rpn_rois, rpn_class_logits, rpn_bbox] =
      PredictRPN(images, config_->post_nms_rois_inference);

  // Network Heads
  // Proposal classifier and BBox regressor heads
  auto [mrcnn_class_logits, mrcnn_class, mrcnn_bbox] =
      classifier_->forward(mrcnn_feature_maps, rpn_rois);

  // Detections
  // output is [batch, num_detections, (y1, x1, y2, x2, class_id, score)] in
  // image coordinates
  at::Tensor detections = DetectionLayer(*config_.get(), rpn_rois, mrcnn_class,
                                         mrcnn_bbox, image_metas);

  // Convert boxes to normalized coordinates
  auto h = static_cast<float>(config_->image_shape[0]);
  auto w = static_cast<float>(config_->image_shape[1]);

  auto scale =
      torch::tensor({h, w, h, w}, at::dtype(at::kFloat).requires_grad(false));

  if (config_->gpu_count > 0)
    scale = scale.cuda();
  auto detection_boxes = detections.narrow(1, 0, 4) / scale;

  // Add back batch dimension
  detection_boxes = detection_boxes.unsqueeze(0);

  // Create masks for detections
  auto mrcnn_mask = mask_->forward(mrcnn_feature_maps, detection_boxes);

  // Add back batch dimension
  detections = detections.unsqueeze(0);
  mrcnn_mask = mrcnn_mask.unsqueeze(0);

  return {detections, mrcnn_mask};
}

// Build Mask R-CNN architecture.
void MaskRCNNImpl::Build() {
  assert(config_);

  // Image size must be dividable by 2 multiple times
  auto h = config_->image_shape[0];
  auto w = config_->image_shape[1];
  auto p = static_cast<int32_t>(std::pow(2l, 6l));
  if (static_cast<int32_t>(static_cast<double>(h) / static_cast<double>(p)) !=
          h / p ||
      static_cast<int32_t>(static_cast<double>(w) / static_cast<double>(p)) !=
          w / p) {
    throw std::invalid_argument(
        "Image size must be dividable by 2 at least 6 times "
        "to avoid fractions when downscaling and upscaling."
        "For example, use 256, 320, 384, 448, 512, ... etc. ");
  }

  // Build the shared convolutional layers.
  // Bottom-up Layers
  // Returns a list of the last layers of each stage, 5 in total.
  // Don't create the thead (stage 5), so we pick the 4th item in the list.
  ResNetImpl resnet(ResNetImpl::Architecture::ResNet101, true);
  auto [C1, C2, C3, C4, C5] = resnet.GetStages();

  // Top-down Layers
  // TODO: add assert to varify feature map sizes match what's in config
  fpn_ = FPN(C1, C2, C3, C4, C5, /*out_channels*/ 256);
  register_module("fpn", fpn_);

  anchors_ = GeneratePyramidAnchors(
      config_->rpn_anchor_scales, config_->rpn_anchor_ratios,
      config_->backbone_shapes, config_->backbone_strides,
      config_->rpn_anchor_stride);

  if (config_->gpu_count > 0)
    anchors_ = anchors_.toBackend(torch::Backend::CUDA);

  // RPN
  rpn_ =
      RPN(config_->rpn_anchor_ratios.size(), config_->rpn_anchor_stride, 256);
  register_module("rpn", rpn_);

  // FPN Classifier
  classifier_ = Classifier(256, config_->pool_size, config_->image_shape,
                           config_->num_classes);
  register_module("classifier", classifier_);

  // FPN Mask
  mask_ = Mask(256, config_->mask_pool_size, config_->image_shape,
               config_->num_classes);
  register_module("mask", mask_);

  // Fix batch norm layers
  auto set_bn_fix = [](const std::string& name, Module& m) {
    if (name.find("BatchNorm") != std::string::npos) {
      for (auto& p : m.parameters())
        p.set_requires_grad(false);
    }
  };

  apply(set_bn_fix);
}

void MaskRCNNImpl::InitializeWeights() {
  for (auto m : modules(false)) {
    if (m->name().find("Conv2d") != std::string::npos) {
      for (auto& p : m->named_parameters()) {
        if (p.key().find("weight") != std::string::npos) {
          torch::nn::init::xavier_uniform_(p.value());
        } else if (p.key().find("bias") != std::string::npos) {
          torch::nn::init::zeros_(p.value());
        }
      }
    } else if (m->name().find("BatchNorm2d") != std::string::npos) {
      for (auto& p : m->named_parameters()) {
        if (p.key().find("weight") != std::string::npos) {
          torch::nn::init::ones_(p.value());
        }
        if (p.key().find("bias") != std::string::npos) {
          torch::nn::init::zeros_(p.value());
        }
      }
    } else if (m->name().find("Linear") != std::string::npos) {
      for (auto& p : m->named_parameters()) {
        if (p.key().find("weight") != std::string::npos) {
          torch::nn::init::normal_(p.value(), 0, 0.01);
        }
        if (p.key().find("bias") != std::string::npos) {
          torch::nn::init::zeros_(p.value());
        }
      }
    }
  }
}

void MaskRCNNImpl::SetTrainableLayers(const std::string& layers_regex) {
  std::regex re(layers_regex);
  std::smatch m;
  auto params = named_parameters(true /*recurse*/);
  for (auto& param : params) {
    auto layer_name = param.key();
    bool is_trainable = std::regex_match(layer_name, m, re);
    if (!is_trainable) {
      param.value().set_requires_grad(false);
    }
  }
}

std::string MaskRCNNImpl::GetCheckpointPath(uint32_t epoch) const {
  return fs::path(model_dir_) /
         ("checkpoint_epoch_" + std::to_string(epoch) + ".pt");
}
