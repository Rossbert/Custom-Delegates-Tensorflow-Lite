#pragma once
// Code based on conv.cc definitions and declarations
// Referencing the source code including conflicting dependencies
// Because of the std namespace conflicts with C:\Users\rosal\tensorflow source\tflite_c_build\gemmlowp
// Which traces the tree to minwindef.h

#include <iostream>
#include <vector>
#include <bitset>
#include <thread>
#include <mutex>
#include <tensorflow/lite/core/c/builtin_op_data.h>
#include <tensorflow/lite/core/c/c_api_types.h>
#include <tensorflow/lite/kernels/internal/tensor_ctypes.h>
#include <tensorflow/lite/kernels/kernel_util.h>
#include <tensorflow/lite/kernels/padding.h>
#include <tensorflow/lite/util.h>
// Necessary for convolutional reference operations
// For Int8 operations
#include <tensorflow/lite/kernels/internal/reference/integer_ops/conv.h>

#include "Options.h"

// All references to TFLITE_WITH_MULTITHREADED_EIGEN are removed, no multithreading
namespace tflite {
	// Forward declaration
	namespace custom_ops {
		namespace conv {
			struct OpData;
		}
	}
	// Declaration
	namespace custom_logger {
		namespace conv {
			void LogTfLiteOpData(const custom_ops::conv::OpData* const data);
		}
		void LogTfLiteConvParams(const TfLiteConvParams* const params);
		void LogTfLiteTensor(const TfLiteTensor& tensor);
	}

	namespace custom_ops {

		namespace conv {

			// This file has 4 implementation of Conv.
			enum KernelType {
				// Normal implementation
				kReference,

				// Neon-free
				kGenericOptimized,

				// kMultithreadOptimized is a mixture of an Eigen-based kernel when threads
				// are available and kGenericOptimized when we must use only one thread.
				kMultithreadOptimized,

				// The kernel uses use CBLAS interface for matrix multiplication.
				// It's fast when an optimized CBLAS implementation is available (e.g. Apple
				// Accelerate Framework), and it's slow when falling back to naive
				// implementation.
				kCblasOptimized,
			};

			// Constat for indication unallocated tensor
			const int kTensorNotAllocated = -1;

			// Max buffer size Mobile
			static constexpr size_t kMaxIm2colBufferSizeMobile = 1024 * 1024 * 1024;  // 1GB

			// Convolution Operation Data
			struct OpData {
				// IDs are the arbitrary identifiers used by TF Lite to identify and access
				// memory buffers.
				int im2col_id = kTensorNotAllocated;
				int hwcn_weights_id = kTensorNotAllocated;
				int input_quantized_id = kTensorNotAllocated;
				int scaling_factors_id = kTensorNotAllocated;
				int input_offset_id = kTensorNotAllocated;
				int accum_scratch_id = kTensorNotAllocated;
				// Row sums are used to cache filter sums for hybrid zero-point calculations.
				int row_sums_id = kTensorNotAllocated;

				TfLitePaddingValues padding;
				// The scaling factor from input to output (aka the 'real multiplier') can
				// be represented as a fixed point multiplier plus a left shift.
				int32_t output_multiplier;
				int output_shift;

				// Per channel output multiplier and shift.
				std::vector<int32_t> per_channel_output_multiplier;
				std::vector<int> per_channel_output_shift;

				// The range of the fused activation layer. For example for kNone and
				// uint8_t these would be 0 and 255.
				int32_t output_activation_min;
				int32_t output_activation_max;
				// Indexes are the offset to the memory buffer in the array used to keep track
				// of the allocated temporaries.
				int32_t im2col_index;
				int32_t hwcn_weights_index;
				int32_t input_quantized_index;
				int32_t scaling_factors_index;
				int32_t accum_scratch_index;
				int32_t input_offset_index;
				int32_t row_sums_index;

				bool need_hwcn_weights = false;
				bool have_weights_been_transposed = false;
				bool need_im2col = false;
				// If it's true, it means im2col is needed but gets disabled because the
				// temporary im2col tensor requires too much memory (i.e.
				// >= kMaxIm2colBufferSize);
				bool im2col_oversized = false;

				bool supports_multithreaded_kernel = false;
				bool is_hybrid_per_channel = false;
				bool compute_hybrid_row_sums = true;

				// Number of convolution groups.
				int32_t groups = 1;

				TfLiteType quantized_bias_type = kTfLiteNoType;
			};

			// inline fuction that returns the PaddingType
			inline PaddingType RuntimePaddingType(TfLitePadding padding) 
			{
				switch (padding) 
				{
				case TfLitePadding::kTfLitePaddingSame:
					return PaddingType::kSame;
				case TfLitePadding::kTfLitePaddingValid:
					return PaddingType::kValid;
				case TfLitePadding::kTfLitePaddingUnknown:
				default:
					return PaddingType::kNone;
				}
			}

			// Init function based on function Init on conv.cc
			// It creates and returns an instance of OpData
			void* Init(TfLiteContext* context, const char* buffer, size_t length);
			
			// Free function based on function Free on conv.cc
			// Frees the memory of the OpData created in init
			void Free(TfLiteContext* context, void* buffer);
			
			// Check if im2col needs to be allocated, as some version of optimized Conv dont
			// use it. If any change is supporting im2col in any of the Conv versions, then
			// it should be updated here as well
			bool IsIm2ColRequired(const TfLiteTensor* input, TfLiteConvParams* params,
				const TfLiteTensor* filter, OpData* data, bool is_hybrid,
				KernelType kernel_type);

			// Allocate temporary tensors (`im2col`, `hwcn_weights` if necessary).
			// Note: `context->AddTensors` might invalidate pointers to existing tensors.
			// Therefore the logic to add tensors are isolated into this function.
			// No reason for this function to be static given that no other function will access this namespace
			TfLiteStatus AllocateTemporaryTensorsIfRequired(
				TfLiteContext* context, TfLiteNode* node, bool is_hybrid,
				bool is_per_channel, KernelType kernel_type, size_t im2col_bytes,
				TfLiteConvParams* params, OpData* data);

			// Prepare function based on Prepare on conv.cc
			// Receives the KernelType so the operation will be selected later
			TfLiteStatus Prepare(KernelType kernel_type, TfLiteContext* context, TfLiteNode* node,
				TfLiteConvParams* params, OpData* data);

			// Templated function based on the function of the same name on conv.cc
			// This function calls the non-templated Prepare function
			template <KernelType kernel_type>
			TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node,
				TfLiteConvParams* params, OpData* data);

			// Templated function based on the function of the same name on conv.cc
			template <KernelType kernel_type>
			void EvalQuantizedPerChannel(TfLiteContext* context, TfLiteNode* node,
				TfLiteConvParams* params, OpData* data,
				const TfLiteTensor* input,
				const TfLiteTensor* filter,
				const TfLiteTensor* bias, TfLiteTensor* output,
				TfLiteTensor* im2col, const MyDelegateOptions& options);

			// Templated function based on the function of the same name on conv.cc
			template <KernelType kernel_type, TfLiteType input_type>
			TfLiteStatus EvalImpl(TfLiteContext* context, TfLiteNode* node,
				TfLiteConvParams* params, OpData* data, const MyDelegateOptions& options);

			// Templated function based on the function of the same name on conv.cc
			template <KernelType kernel_type>
			TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node,
				TfLiteConvParams* params, OpData* data, const MyDelegateOptions& options);

			// Fixed-point per-channel-quantization convolution reference kernel.
			inline void ConvPerChannel(
				const ConvParams& params, const int32_t* output_multiplier,
				const int32_t* output_shift, const RuntimeShape& input_shape,
				const int8_t* input_data, const RuntimeShape& filter_shape,
				const int8_t* filter_data, const RuntimeShape& bias_shape,
				const int32_t* bias_data, const RuntimeShape& output_shape,
				int8_t* output_data, const MyDelegateOptions& options)
			{
				// Get parameters.
				const int32_t input_offset = params.input_offset;  // r = s(q - Z)
				const int stride_width = params.stride_width;
				const int stride_height = params.stride_height;
				const int dilation_width_factor = params.dilation_width_factor;
				const int dilation_height_factor = params.dilation_height_factor;
				const int pad_width = params.padding_values.width;
				const int pad_height = params.padding_values.height;
				const int32_t output_offset = params.output_offset;

				// Set min and max value of the output.
				const int32_t output_activation_min = params.quantized_activation_min;
				const int32_t output_activation_max = params.quantized_activation_max;

				// Consistency check.
				TFLITE_DCHECK_LE(output_activation_min, output_activation_max);
				TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
				TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
				TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);
				const int batches = MatchingDim(input_shape, 0, output_shape, 0);
				const int input_depth = input_shape.Dims(3);
				const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);
				if (bias_data)
				{
					TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
				}

				// Check dimensions of the tensors.
				const int input_height = input_shape.Dims(1);
				const int input_width = input_shape.Dims(2);
				const int filter_height = filter_shape.Dims(1);
				const int filter_width = filter_shape.Dims(2);
				const int filter_input_depth = filter_shape.Dims(3);
				const int groups = input_depth / filter_input_depth;
				TFLITE_DCHECK_NE(groups, 0);
				TFLITE_DCHECK_EQ(input_depth % filter_input_depth, 0);
				const int filters_per_group = output_depth / groups;
				TFLITE_DCHECK_NE(filters_per_group, 0);
				const int output_height = output_shape.Dims(1);
				const int output_width = output_shape.Dims(2);

				// 1 For some reason tensor allocate only allows 1 image to be analyzed
				for (int batch = 0; batch < batches; ++batch) 
				{
					// 
					for (int out_y = 0; out_y < output_height; ++out_y) 
					{
						const int in_y_origin = (out_y * stride_height) - pad_height;
						// 
						for (int out_x = 0; out_x < output_width; ++out_x) 
						{
							const int in_x_origin = (out_x * stride_width) - pad_width;
							// 
							for (int out_channel = 0; out_channel < output_depth; ++out_channel) 
							{
								// Will always be 0!!!!!!! input channels = filter input channels then filters per group = number of filters (output channels) so group = 0
								auto group = out_channel / filters_per_group;

								int32_t acc = 0;
								for (int filter_y = 0; filter_y < filter_height; ++filter_y) 
								{
									const int in_y = in_y_origin + dilation_height_factor * filter_y;
									for (int filter_x = 0; filter_x < filter_width; ++filter_x) 
									{
										const int in_x = in_x_origin + dilation_width_factor * filter_x;

										// Zero padding by omitting the areas outside the image.
										const bool is_point_inside_image = 
											(in_x >= 0) && (in_x < input_width) && 
											(in_y >= 0) && (in_y < input_height);

										if (!is_point_inside_image) 
										{
											continue;
										}

										for (int in_channel = 0; in_channel < filter_input_depth; ++in_channel) 
										{
											int32_t input_val = input_data[Offset(input_shape, batch, in_y, in_x, in_channel + group * filter_input_depth)];
											int32_t filter_val = filter_data[Offset(filter_shape, out_channel, filter_y, filter_x, in_channel)];
											// Accumulate with 32 bits accumulator.
											// In the nudging process during model quantization, we force
											// real value of 0.0 be represented by a quantized value. This
											// guarantees that the input_offset is a int8_t, even though
											// it is represented using int32_t. int32_t += int8_t *
											// (int8_t - int8_t) so the highest value we can get from each
											// accumulation is [-127, 127] * ([-128, 127] -
											// [-128, 127]), which is [-32512, 32512]. log2(32512)
											// = 14.98, which means we can accumulate at least 2^16
											// multiplications without overflow. The accumulator is
											// applied to a filter so the accumulation logic will hold as
											// long as the filter size (filter_y * filter_x * in_channel)
											// does not exceed 2^16, which is the case in all the models
											// we have seen so far.
											// TODO(b/174275578): Add a check to make sure the
											// accumulator depth is smaller than 2^16.
											acc += filter_val * (input_val + input_offset);
										}
									}
								}

								// Here is the point where the previous python flipper version carried the bit flipping
								if (bias_data) 
								{
									acc += bias_data[out_channel];
								}
								acc = MultiplyByQuantizedMultiplier(acc, output_multiplier[out_channel], output_shift[out_channel]);
								acc += output_offset;
								acc = std::max(acc, output_activation_min);
								acc = std::min(acc, output_activation_max);
								output_data[Offset(output_shape, batch, out_y, out_x, out_channel)] = static_cast<int8_t>(acc);
							}
						}
					}
				}
			}
			
			// Raw operation to pararellize in threads
			inline void DisturbedConvolutionOperation(
				const int32_t* output_multiplier, const int32_t* output_shift,
				const int batches, const int output_height, const int output_width, const int output_depth,
				const int filter_height, const int filter_width, const int filter_input_depth,
				const int stride_height, const int pad_height,
				const int stride_width, const int pad_width,
				const int input_height, const int input_width,
				const int filters_per_group,
				const int dilation_width_factor, const int dilation_height_factor,
				const int input_offset, const int output_offset,
				const int output_activation_min, const int output_activation_max,
				const RuntimeShape& input_shape, const int8_t* input_data,
				const RuntimeShape& filter_shape, const int8_t* filter_data,
				const RuntimeShape& bias_shape, const int32_t* bias_data,
				const RuntimeShape& output_shape, int8_t* output_data,
				const std::vector<int>& chunk_indexes,
				const MyDelegateOptions& options)
			{
				const int& dataset_index = options.dataset_index;
				int idx_counter = chunk_indexes.size() - 1;
				// 1 For some reason tensor allocate only allows 1 image to be analyzed
				for (int batch = 0; batch < batches; ++batch)
				{
					// 
					for (int out_y = 0; out_y < output_height; ++out_y)
					{
						const int in_y_origin = (out_y * stride_height) - pad_height;
						// 
						for (int out_x = 0; out_x < output_width; ++out_x)
						{
							const int in_x_origin = (out_x * stride_width) - pad_width;
							// 
							for (int out_channel = 0; out_channel < output_depth; ++out_channel)
							{
								int outputPosition = batch * output_height * output_width * output_depth + out_y * output_width * output_depth + out_x * output_depth + out_channel;

								// Will always be 0!!!!!!! input channels = filter input channels then filters per group = number of filters (output channels) so group = 0
								auto group = out_channel / filters_per_group;

								int32_t acc = 0;
								for (int filter_y = 0; filter_y < filter_height; ++filter_y)
								{
									const int in_y = in_y_origin + dilation_height_factor * filter_y;
									for (int filter_x = 0; filter_x < filter_width; ++filter_x)
									{
										const int in_x = in_x_origin + dilation_width_factor * filter_x;

										// Zero padding by omitting the areas outside the image.
										const bool is_point_inside_image =
											(in_x >= 0) && (in_x < input_width) &&
											(in_y >= 0) && (in_y < input_height);

										if (!is_point_inside_image)
										{
											continue;
										}

										for (int in_channel = 0; in_channel < filter_input_depth; ++in_channel)
										{
											int kernelPartialPosition = filter_y * filter_width * filter_input_depth + filter_x * filter_input_depth + in_channel;

											int32_t input_val = input_data[Offset(input_shape, batch, in_y, in_x, in_channel + group * filter_input_depth)];
											int32_t filter_val = filter_data[Offset(filter_shape, out_channel, filter_y, filter_x, in_channel)];

											int32_t result = filter_val * (input_val + input_offset);

											if (idx_counter >= 0 && options.error_flat_positions[dataset_index][chunk_indexes[idx_counter]].first == outputPosition && options.error_flat_positions[dataset_index][chunk_indexes[idx_counter]].second == kernelPartialPosition)
											{
												std::bitset<32> bits(result);
												bits.flip(options.bit_position);
												result = static_cast<int>(bits.to_ulong());
												idx_counter--;
											}

											// Accumulate with 32 bits accumulator.
											// In the nudging process during model quantization, we force
											// real value of 0.0 be represented by a quantized value. This
											// guarantees that the input_offset is a int8_t, even though
											// it is represented using int32_t. int32_t += int8_t *
											// (int8_t - int8_t) so the highest value we can get from each
											// accumulation is [-127, 127] * ([-128, 127] -
											// [-128, 127]), which is [-32512, 32512]. log2(32512)
											// = 14.98, which means we can accumulate at least 2^16
											// multiplications without overflow. The accumulator is
											// applied to a filter so the accumulation logic will hold as
											// long as the filter size (filter_y * filter_x * in_channel)
											// does not exceed 2^16, which is the case in all the models
											// we have seen so far.
											// TODO(b/174275578): Add a check to make sure the
											// accumulator depth is smaller than 2^16.
											acc += result;
										}
									}
								}

								if (bias_data)
								{
									acc += bias_data[out_channel];
								}
								acc = MultiplyByQuantizedMultiplier(acc, output_multiplier[out_channel], output_shift[out_channel]);
								acc += output_offset;
								acc = std::max(acc, output_activation_min);
								acc = std::min(acc, output_activation_max);
								output_data[Offset(output_shape, batch, out_y, out_x, out_channel)] = static_cast<int8_t>(acc);
							}
						}
					}
				}
			}
			
			
			// Raw operation to pararellize in threads
			inline void DisturbedConvolutionOperationByChunks(
				const int start_chunk, const int end_chunk,
				const int32_t* output_multiplier, const int32_t* output_shift,
				const int batches, const int output_height, const int output_width, const int output_depth,
				const int filter_height, const int filter_width, const int filter_input_depth,
				const int stride_height, const int pad_height,
				const int stride_width, const int pad_width,
				const int input_height, const int input_width,
				const int filters_per_group,
				const int dilation_width_factor, const int dilation_height_factor,
				const int input_offset, const int output_offset,
				const int output_activation_min, const int output_activation_max,
				const RuntimeShape& input_shape, const int8_t* input_data,
				const RuntimeShape& filter_shape, const int8_t* filter_data,
				const RuntimeShape& bias_shape, const int32_t* bias_data,
				const RuntimeShape& output_shape, int8_t* output_data,
				const std::vector<int>& chunk_indexes,
				const MyDelegateOptions& options)
			{
				const int& dataset_index = options.dataset_index;
				int idx_counter = chunk_indexes.size() - 1;
				// 1 For some reason tensor allocate only allows 1 image to be analyzed
				for (int batch = 0; batch < batches; ++batch)
				{
					// 
					for (int out_y = 0; out_y < output_height; ++out_y)
					{
						const int in_y_origin = (out_y * stride_height) - pad_height;
						// 
						for (int out_x = 0; out_x < output_width; ++out_x)
						{
							const int in_x_origin = (out_x * stride_width) - pad_width;
							// 
							for (int out_channel = start_chunk; out_channel < end_chunk; ++out_channel)
							{
								int outputPosition = batch * output_height * output_width * output_depth + out_y * output_width * output_depth + out_x * output_depth + out_channel;

								// Will always be 0!!!!!!! input channels = filter input channels then filters per group = number of filters (output channels) so group = 0
								auto group = out_channel / filters_per_group;

								int32_t acc = 0;
								for (int filter_y = 0; filter_y < filter_height; ++filter_y)
								{
									const int in_y = in_y_origin + dilation_height_factor * filter_y;
									for (int filter_x = 0; filter_x < filter_width; ++filter_x)
									{
										const int in_x = in_x_origin + dilation_width_factor * filter_x;

										// Zero padding by omitting the areas outside the image.
										const bool is_point_inside_image =
											(in_x >= 0) && (in_x < input_width) &&
											(in_y >= 0) && (in_y < input_height);

										if (!is_point_inside_image)
										{
											continue;
										}

										for (int in_channel = 0; in_channel < filter_input_depth; ++in_channel)
										{
											int kernelPartialPosition = filter_y * filter_width * filter_input_depth + filter_x * filter_input_depth + in_channel;

											int32_t input_val = input_data[Offset(input_shape, batch, in_y, in_x, in_channel + group * filter_input_depth)];
											int32_t filter_val = filter_data[Offset(filter_shape, out_channel, filter_y, filter_x, in_channel)];

											int32_t result = filter_val * (input_val + input_offset);

											if (idx_counter >= 0 && options.error_flat_positions[dataset_index][chunk_indexes[idx_counter]].first == outputPosition && options.error_flat_positions[dataset_index][chunk_indexes[idx_counter]].second == kernelPartialPosition)
											{
												//std::unique_lock<std::mutex> lock(coutMutex);
												//if (dataset_index == 0)
												//{
												//	std::cout << "Index counter " << idx_counter << "\n";
												//}
												//lock.unlock();
												std::bitset<32> bits(result);
												bits.flip(options.bit_position);
												result = static_cast<int>(bits.to_ulong());
												idx_counter--;
											}

											// Accumulate with 32 bits accumulator.
											// In the nudging process during model quantization, we force
											// real value of 0.0 be represented by a quantized value. This
											// guarantees that the input_offset is a int8_t, even though
											// it is represented using int32_t. int32_t += int8_t *
											// (int8_t - int8_t) so the highest value we can get from each
											// accumulation is [-127, 127] * ([-128, 127] -
											// [-128, 127]), which is [-32512, 32512]. log2(32512)
											// = 14.98, which means we can accumulate at least 2^16
											// multiplications without overflow. The accumulator is
											// applied to a filter so the accumulation logic will hold as
											// long as the filter size (filter_y * filter_x * in_channel)
											// does not exceed 2^16, which is the case in all the models
											// we have seen so far.
											// TODO(b/174275578): Add a check to make sure the
											// accumulator depth is smaller than 2^16.
											acc += result;
										}
									}
								}

								if (bias_data)
								{
									acc += bias_data[out_channel];
								}
								acc = MultiplyByQuantizedMultiplier(acc, output_multiplier[out_channel], output_shift[out_channel]);
								acc += output_offset;
								acc = std::max(acc, output_activation_min);
								acc = std::min(acc, output_activation_max);
								output_data[Offset(output_shape, batch, out_y, out_x, out_channel)] = static_cast<int8_t>(acc);
							}
						}
					}
				}
			}

			inline void ParallelDisturbedConvolution(
				const int32_t* output_multiplier, const int32_t* output_shift,
				const int batches, const int output_height, const int output_width, const int output_depth,
				const int filter_height, const int filter_width, const int filter_input_depth,
				const int stride_height, const int pad_height,
				const int stride_width, const int pad_width,
				const int input_height, const int input_width,
				const int filters_per_group,
				const int dilation_width_factor, const int dilation_height_factor,
				const int input_offset, const int output_offset,
				const int output_activation_min, const int output_activation_max,
				const RuntimeShape& input_shape, const int8_t* input_data,
				const RuntimeShape& filter_shape, const int8_t* filter_data,
				const RuntimeShape& bias_shape, const int32_t* bias_data,
				const RuntimeShape& output_shape, int8_t* output_data,
				const MyDelegateOptions& options)
			{
				std::vector<std::thread> threadPool;
				//std::mutex coutMutex;

				for (int i = 0; i < options.num_threads; ++i)
				{
					const int start = i * options.chunk_size;
					const int end = std::min(start + options.chunk_size, options.channels);

#if LOGGER
					//if (dataset_index == 2)
					//{
					//	std::cout << "Indexes size " << options.chunks_indexes.size() << "\n";
					//	std::cout << "Indexes capacity " << options.chunks_indexes.capacity() << "\n";
					//	std::cout << "Start: " << start << " End: " << end << "\n";
					//	std::cout << "Indexes in chunk " << i << ": ";
					//	for (const auto& val : options.chunks_indexes[dataset_index][i])
					//	{
					//		std::cout << val << " ";
					//	}
					//	std::cout << "\n";
					//}
					
					//std::cout << "Real positions\n";
					//for (const auto& val : options.error_vec_positions[dataset_index])
					//{
					//	for (const int& element : val.first)
					//	{
					//		std::cout << element << " ";
					//	}
					//	std::cout << "- ";
					//	for (const int& element : val.second)
					//	{
					//		std::cout << element << " ";
					//	}
					//	std::cout << "\n";
					//}
					//std::cout << "\n";
#endif // LOGGER

					threadPool.emplace_back(
						DisturbedConvolutionOperationByChunks,
						start, end,
						output_multiplier, output_shift,
						batches, output_height, output_width, output_depth,
						filter_height, filter_width, filter_input_depth,
						stride_height, pad_height,
						stride_width, pad_width,
						input_height, input_width,
						filters_per_group,
						dilation_width_factor, dilation_height_factor,
						input_offset, output_offset,
						output_activation_min, output_activation_max,
						std::cref(input_shape), input_data,
						std::cref(filter_shape), filter_data,
						std::cref(bias_shape), bias_data,
						std::cref(output_shape), output_data,
						std::cref(options.chunks_indexes[options.dataset_index][i]),
						std::cref(options));
				}

				// Join all threads
				for (auto& thread : threadPool)
				{
					thread.join();
				}	
			}

			// Fixed-point per-channel-quantization convolution reference kernel.
			inline void ConvPerChannelDisturbed(
				const ConvParams& params, 
				const int32_t* output_multiplier, const int32_t* output_shift, 
				const RuntimeShape& input_shape, const int8_t* input_data, 
				const RuntimeShape& filter_shape, const int8_t* filter_data, 
				const RuntimeShape& bias_shape, const int32_t* bias_data, 
				const RuntimeShape& output_shape, int8_t* output_data, 
				const MyDelegateOptions& options)
			{
				// Get parameters.
				const int32_t input_offset = params.input_offset;  // r = s(q - Z)
				const int stride_width = params.stride_width;
				const int stride_height = params.stride_height;
				const int dilation_width_factor = params.dilation_width_factor;
				const int dilation_height_factor = params.dilation_height_factor;
				const int pad_width = params.padding_values.width;
				const int pad_height = params.padding_values.height;
				const int32_t output_offset = params.output_offset;

				// Set min and max value of the output.
				const int32_t output_activation_min = params.quantized_activation_min;
				const int32_t output_activation_max = params.quantized_activation_max;

				// Consistency check.
				TFLITE_DCHECK_LE(output_activation_min, output_activation_max);
				TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
				TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
				TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);
				const int batches = MatchingDim(input_shape, 0, output_shape, 0);
				const int input_depth = input_shape.Dims(3);
				const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);
				if (bias_data)
				{
					TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
				}

				// Check dimensions of the tensors.
				const int input_height = input_shape.Dims(1);
				const int input_width = input_shape.Dims(2);
				const int filter_height = filter_shape.Dims(1);
				const int filter_width = filter_shape.Dims(2);
				const int filter_input_depth = filter_shape.Dims(3);
				const int groups = input_depth / filter_input_depth;
				TFLITE_DCHECK_NE(groups, 0);
				TFLITE_DCHECK_EQ(input_depth % filter_input_depth, 0);
				const int filters_per_group = output_depth / groups;
				TFLITE_DCHECK_NE(filters_per_group, 0);
				const int output_height = output_shape.Dims(1);
				const int output_width = output_shape.Dims(2);
				
				if (options.is_threaded)
				{
					// Parallel computing done here!
					ParallelDisturbedConvolution(
						output_multiplier, output_shift,
						batches, output_height, output_width, output_depth,
						filter_height, filter_width, filter_input_depth,
						stride_height, pad_height,
						stride_width, pad_width,
						input_height, input_width,
						filters_per_group,
						dilation_width_factor, dilation_height_factor,
						input_offset, output_offset,
						output_activation_min, output_activation_max,
						input_shape, input_data,
						filter_shape, filter_data,
						bias_shape, bias_data,
						output_shape, output_data,
						options);
				}
				else
				{
					DisturbedConvolutionOperation(
						output_multiplier, output_shift,
						batches, output_height, output_width, output_depth,
						filter_height, filter_width, filter_input_depth,
						stride_height, pad_height,
						stride_width, pad_width,
						input_height, input_width,
						filters_per_group,
						dilation_width_factor, dilation_height_factor,
						input_offset, output_offset,
						output_activation_min, output_activation_max,
						input_shape, input_data,
						filter_shape, filter_data,
						bias_shape, bias_data,
						output_shape, output_data,
						options.full_indexes,
						options);
				}

				/*
				// 1 For some reason tensor allocate only allows 1 image to be analyzed
				int idx_counter = options.full_indexes.size() - 1;
				for (int batch = 0; batch < batches; ++batch)
				{
					// 
					for (int out_y = 0; out_y < output_height; ++out_y)
					{
						const int in_y_origin = (out_y * stride_height) - pad_height;
						// 
						for (int out_x = 0; out_x < output_width; ++out_x)
						{
							const int in_x_origin = (out_x * stride_width) - pad_width;
							// 
							//for (int out_channel = start; out_channel < end; ++out_channel)
							for (int out_channel = 0; out_channel < output_depth; ++out_channel)
							{
								int outputPosition = batch * output_height * output_width * output_depth + out_y * output_width * output_depth + out_x * output_depth + out_channel;

								// Will always be 0!!!!!!! input channels = filter input channels then filters per group = number of filters (output channels) so group = 0
								auto group = out_channel / filters_per_group;

								int32_t acc = 0;
								for (int filter_y = 0; filter_y < filter_height; ++filter_y)
								{
									const int in_y = in_y_origin + dilation_height_factor * filter_y;
									for (int filter_x = 0; filter_x < filter_width; ++filter_x)
									{
										const int in_x = in_x_origin + dilation_width_factor * filter_x;

										// Zero padding by omitting the areas outside the image.
										const bool is_point_inside_image =
											(in_x >= 0) && (in_x < input_width) &&
											(in_y >= 0) && (in_y < input_height);

										if (!is_point_inside_image)
										{
											continue;
										}

										for (int in_channel = 0; in_channel < filter_input_depth; ++in_channel)
										{
											int kernelPartialPosition = filter_y * filter_width * filter_input_depth + filter_x * filter_input_depth + in_channel;

											int32_t input_val = input_data[Offset(input_shape, batch, in_y, in_x, in_channel + group * filter_input_depth)];
											int32_t filter_val = filter_data[Offset(filter_shape, out_channel, filter_y, filter_x, in_channel)];

											int32_t result = filter_val * (input_val + input_offset);

											if (idx_counter >= 0 && options.error_flat_positions[options.dataset_index][options.full_indexes[idx_counter]].first == outputPosition && options.error_flat_positions[options.dataset_index][options.full_indexes[idx_counter]].second == kernelPartialPosition)
											{
												std::bitset<32> bits(result);
												bits.flip(options.bit_position);
												result = static_cast<int>(bits.to_ulong());
												idx_counter--;
											}

											// Accumulate with 32 bits accumulator.
											// In the nudging process during model quantization, we force
											// real value of 0.0 be represented by a quantized value. This
											// guarantees that the input_offset is a int8_t, even though
											// it is represented using int32_t. int32_t += int8_t *
											// (int8_t - int8_t) so the highest value we can get from each
											// accumulation is [-127, 127] * ([-128, 127] -
											// [-128, 127]), which is [-32512, 32512]. log2(32512)
											// = 14.98, which means we can accumulate at least 2^16
											// multiplications without overflow. The accumulator is
											// applied to a filter so the accumulation logic will hold as
											// long as the filter size (filter_y * filter_x * in_channel)
											// does not exceed 2^16, which is the case in all the models
											// we have seen so far.
											// TODO(b/174275578): Add a check to make sure the
											// accumulator depth is smaller than 2^16.
											acc += result;
										}
									}
								}

								// Here is the point where the previous python flipper version carried the bit flipping
								if (bias_data)
								{
									acc += bias_data[out_channel];
								}
								acc = MultiplyByQuantizedMultiplier(acc, output_multiplier[out_channel], output_shift[out_channel]);
								acc += output_offset;
								acc = std::max(acc, output_activation_min);
								acc = std::min(acc, output_activation_max);
								output_data[Offset(output_shape, batch, out_y, out_x, out_channel)] = static_cast<int8_t>(acc);
							}
						}
					}
				}
				*/

			}

		}
	
		// Gets the input, filter, and output indexes if the order of tensor inputs is mixed
		void GetTensorIndexes(TfLiteContext* context, TfLiteNode* node,
			int* bias_index, int* filter_index, int* input_index);

		// Extract the number of total elements inside a TfLiteIntArray
		int getFlatSize(const TfLiteIntArray* dimensions);

		// Extract the number of total elements inside a TfLiteIntArray
		int getFlatSize(const TfLiteIntArray* dimensions, const int starting_index);

	}
}

#include "ConvTemplates.h"