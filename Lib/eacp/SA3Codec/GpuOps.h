#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <vector>

namespace eacp::SA3Codec
{
ML::Tensor reshapeFlat(ML::Tensor tensor, std::vector<int> newShape);

ML::Tensor sliceRowsGpu(GPU::ComputePass& pass,
                        const ML::Tensor& input,
                        int startRow,
                        int rowCount,
                        GPU::Device& device = GPU::Device::shared());

ML::Tensor sliceColumnsGpu(GPU::ComputePass& pass,
                           const ML::Tensor& input,
                           int startColumn,
                           int columnCount,
                           GPU::Device& device = GPU::Device::shared());

void writeRowsIntoGpu(GPU::ComputePass& pass,
                      const ML::Tensor& destination,
                      int destinationRowOffset,
                      const ML::Tensor& source,
                      GPU::Device& device = GPU::Device::shared());

ML::Tensor zerosGpu(GPU::ComputePass& pass,
                    int rows,
                    int columns,
                    GPU::Device& device = GPU::Device::shared());

ML::Tensor zeroPadRowsGpu(GPU::ComputePass& pass,
                          const ML::Tensor& input,
                          int multiple,
                          GPU::Device& device = GPU::Device::shared());

ML::Tensor concatRowsGpu(GPU::ComputePass& pass,
                         const ML::Tensor& a,
                         const ML::Tensor& b,
                         const ML::Tensor& c,
                         GPU::Device& device = GPU::Device::shared());

ML::Tensor addTensorsGpu(GPU::ComputePass& pass,
                         const ML::Tensor& a,
                         const ML::Tensor& b,
                         GPU::Device& device = GPU::Device::shared());

ML::Tensor subtractTensorsGpu(GPU::ComputePass& pass,
                              const ML::Tensor& a,
                              const ML::Tensor& b,
                              GPU::Device& device = GPU::Device::shared());

ML::Tensor foldWithNewTokensGpu(GPU::ComputePass& pass,
                                const ML::Tensor& input,
                                int inputSegSize,
                                int outputSegSize,
                                const ML::Tensor& newTokens,
                                GPU::Device& device = GPU::Device::shared());

ML::Tensor unfoldLastSegmentGpu(GPU::ComputePass& pass,
                                const ML::Tensor& input,
                                int subChunkSize,
                                int outputSegSize,
                                GPU::Device& device = GPU::Device::shared());

ML::Tensor conv1dUnfoldGpu(GPU::ComputePass& pass,
                           const ML::Tensor& input,
                           int inChannels,
                           int kernelSize,
                           GPU::Device& device = GPU::Device::shared());
}
