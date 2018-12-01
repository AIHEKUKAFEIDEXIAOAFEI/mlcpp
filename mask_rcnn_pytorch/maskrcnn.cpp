#include "maskrcnn.h"
#include "anchors.h"
#include "resnet.h"

#include <cmath>

MaskRCNN::MaskRCNN(std::string model_dir, std::shared_ptr<Config const> config)
    : model_dir_(model_dir), config_(config) {
  Build();
  InitializeWeights();
}

bool MaskRCNN::Detect(const at::Tensor& image) {
  return false;
}

// Build Mask R-CNN architecture.
void MaskRCNN::Build() {
  assert(config_);

  // Image size must be dividable by 2 multiple times
  auto h = config_->image_shape[0];
  auto w = config_->image_shape[1];
  auto p = static_cast<uint32_t>(std::pow(2l, 6l));
  if (static_cast<uint32_t>(static_cast<double>(h) / static_cast<double>(p)) !=
          h / p ||
      static_cast<uint32_t>(static_cast<double>(w) / static_cast<double>(p)) !=
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
  /*
      // FPN Classifier
      self.classifier =
        Classifier(256, config.POOL_SIZE, config.IMAGE_SHAPE,
     config.NUM_CLASSES);

      // FPN Mask
      self.mask =
        Mask(256, config.MASK_POOL_SIZE, config.IMAGE_SHAPE,
     config.NUM_CLASSES);

      // Fix batch norm layers
      def set_bn_fix(m):
        classname = m.__class__.__name__
        if classname.find('BatchNorm') != -1:
            for p in m.parameters(): p.requires_grad = False

      self.apply(set_bn_fix);
        */
}

void MaskRCNN::InitializeWeights() {}
