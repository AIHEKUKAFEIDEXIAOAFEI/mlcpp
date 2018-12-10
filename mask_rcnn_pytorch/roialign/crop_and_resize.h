#ifndef CROP_AND_RESIZE_H
#define CROP_AND_RESIZE_H

#include <torch/torch.h>

void crop_and_resize_forward(at::Tensor image,
                             at::Tensor boxes,      // [y1, x1, y2, x2]
                             at::Tensor box_index,  // range in [0, batch_size)
                             float extrapolation_value,
                             uint32_t crop_height,
                             uint32_t crop_width,
                             at::Tensor crops);

void crop_and_resize_backward(
    at::Tensor grads,
    at::Tensor boxes,       // [y1, x1, y2, x2]
    at::Tensor box_index,   // range in [0, batch_size)
    at::Tensor grads_image  // resize to [bsize, c, hc, wc]
);

#endif
