#include <torch/extension.h>
using namespace torch;

#include <vector>

#define WITHIN_BOUNDS(x, y, H, W) (x >= 0 && x < H && y >= 0 && y < W)

template <typename scalar_t>
static void correlate_patch_3D(
    TensorAccessor<scalar_t,4> input1,
    TensorAccessor<scalar_t,4> input2,
    scalar_t *dst,
    int kH, int kW, kD,
    int dilationH, int dilationW, int dialationD,
    int u, int v, int w,
    int shiftU, int shiftV, int shiftW){
  const int C = input1.size(0);
  const int iH = input1.size(1);
  const int iW = input1.size(2);
  const int iD = input1.size(3);
  for (int c=0; c<C; ++c){
    for (int i=0; i<kH; ++i){
      int i1 = u + i * dilationH;
      int i2 = i1 + shiftU;
      if WITHIN_BOUNDS(i1, i2, iH, iH){
        for (int j=0; j<kW; ++j){
          int j1 = v + j * dilationW;
          int j2 = j1 + shiftV;
          if WITHIN_BOUNDS(j1, j2, iW, iW){
            for (int k=0; k < kD; ++k){
                int k1 = w + k * dialationD;
                int k2 = k1 + shiftW;
                if WITHIN_BOUNDS(k1, k2, iD, iD){
                    scalar_t v1 = input1[c][i1][j1][k1];
                    scalar_t v2 = input2[c][i2][j2][k2];
                    *dst += v1 * v2;
                }
            }
          }
        }
      }
    }
  }
}

template <typename scalar_t>
static void correlate_patch_grad_3D(
    TensorAccessor<scalar_t,4> input1,
    TensorAccessor<scalar_t,4> gradInput1,
    TensorAccessor<scalar_t,4> input2,
    TensorAccessor<scalar_t,4> gradInput2,
    scalar_t gradOutput,
    int kH, int kW, int kD,
    int dilationH, int dilationW, int dialationD,
    int u, int v, int w,
    int shiftU, int shiftV, int shiftW){

  const int C = input1.size(0);
  const int iH = input1.size(1);
  const int iW = input1.size(2);
  const int iD = input1.size(3);
  for (int c=0; c<C; ++c){
    for (int i=0; i<kH; ++i){
      int i1 = u + i * dilationH;
      int i2 = i1 + shiftU;
      if WITHIN_BOUNDS(i1, i2, iH, iH){
        for (int j=0; j<kW; ++j){
          int j1 = v + j * dilationW;
          int j2 = j1 + shiftV;
          if WITHIN_BOUNDS(j1, j2, iW, iW){
            for (int k=0; k<kD; ++k){
              int k1 = w + k * dilationD;
              int k2 = k1 + shiftW;
              if WITHIN_BOUNDS(k1, k2, iD, iD){
                scalar_t v1 = input1[c][i1][j1][k1];
                scalar_t v2 = input2[c][i2][j2][k2];
                gradInput2[c][i2][j2][k2]+= gradOutput * v1;
                gradInput1[c][i1][j1][k1]+= gradOutput * v2;
          }
        }
      }
    }
  }
}

torch::Tensor correlation_cpp_forward_3D(
    torch::Tensor input1,
    torch::Tensor input2,
    int kH, int kW, int kW
    int patchH, int patchW, int patchD,
    int padH, int padW, int padD,
    int dilationH, int dilationW, int dialationD,
    int dilation_patchH, int dilation_patchW, int dialation_patchD,
    int dH, int dW, int dD) {

  const auto batch_size = input1.size(0);
  const auto iH = input1.size(2);
  const auto iW = input1.size(3);
  const auto iD = input1.size(4);
  const int patchRadH = (patchH - 1) / 2;
  const int patchRadW = (patchW - 1) / 2;
  const int patchRadD = (patchD - 1) / 2;
  const int dilatedKH = (kH - 1) * dilationH + 1;
  const int dilatedKW = (kW - 1) * dilationW + 1;
  const int dilatedKD = (kD - 1) * dilationD + 1;

  const auto oH = (iH + 2 * padH - dilatedKH) / dH + 1;
  const auto oW = (iW + 2 * padW - dilatedKW) / dW + 1;
  const auto oD = (iD + 2 * padD - dilatedKD) / dD + 1;
  auto output = at::zeros({batch_size, patchH, patchW, patchD, oH, oW, oD}, input1.options());

  int n, ph, pw, pd, h, w, d;
  #pragma omp parallel for private(n, ph, pw, h, w) collapse(2)
    for (n = 0; n < batch_size; ++n) {
      for(ph = 0; ph < patchH; ++ph){
        for(pw = 0; pw < patchW; ++pw){
            for(pd = 0; pd < patchD; ++pd){
          AT_DISPATCH_FLOATING_TYPES(input1.scalar_type(), "correlation_forward_cpp", ([&] {
            auto input1_acc = input1.accessor<scalar_t, 4>();
            auto input2_acc = input2.accessor<scalar_t, 4>();
            auto output_acc = output.accessor<scalar_t, 5>();
            for (h = 0; h < oH; ++h) {
              for (w = 0; w < oW; ++w) {
                correlate_patch(input1_acc[n],
                                input2_acc[n],
                                &output_acc[n][ph][pw][h][w],
                                kH, kW,
                                dilationH, dilationW,
                                -padH + h * dH,
                                -padW + w * dW,
                                (ph - patchRadH)  * dilation_patchH,
                                (pw - patchRadW)  * dilation_patchW);
              }
            }
          }));
        }
      }
    }
  return output;
}

std::vector<torch::Tensor> correlation_cpp_backward(
    torch::Tensor input1,
    torch::Tensor input2,
    torch::Tensor gradOutput,
    int kH, int kW,
    int patchH, int patchW,
    int padH, int padW,
    int dilationH, int dilationW,
    int dilation_patchH, int dilation_patchW,
    int dH, int dW) {
  
  const int batch_size = input1.size(0);
  const int patchRadH = (patchH - 1) / 2;
  const int patchRadW = (patchW - 1) / 2;
  const int oH = gradOutput.size(3);
  const int oW = gradOutput.size(4);
  
  auto gradInput1 = torch::zeros_like(input1);

  auto gradInput2 = torch::zeros_like(input2);

  int n, ph, pw, h, w;
  #pragma omp parallel for private(n, ph, pw, h, w)
    for (n = 0; n < batch_size; ++n) {
      AT_DISPATCH_FLOATING_TYPES(input1.scalar_type(), "correlation_backward_cpp", ([&] {
        auto input1_acc = input1.accessor<scalar_t, 4>();
        auto gradInput1_acc = gradInput1.accessor<scalar_t, 4>();
        auto input2_acc = input2.accessor<scalar_t, 4>();
        auto gradInput2_acc = gradInput2.accessor<scalar_t, 4>();
        auto gradOutput_acc = gradOutput.accessor<scalar_t, 5>();

        for(ph = 0; ph < patchH; ++ph){
          for(pw = 0; pw < patchW; ++pw){
            for (h = 0; h < oH; ++h) {
              for (w = 0; w < oW; ++w) {
                correlate_patch_grad(input1_acc[n], gradInput1_acc[n],
                                     input2_acc[n], gradInput2_acc[n],
                                     gradOutput_acc[n][ph][pw][h][w],
                                     kH, kW,
                                     dilationH, dilationW,
                                     -padH + h * dH,
                                     -padW + w * dW,
                                     (ph - patchRadH)  * dilation_patchH,
                                     (pw - patchRadW)  * dilation_patchW);
              }
            }
          }
        }
      }));
    }

  return {gradInput1, gradInput2};
}
