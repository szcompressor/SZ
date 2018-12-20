#include "sz_opencl.h"
#include "sz.h"
#include <algorithm>
#include <iterator>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#define __CL_ENABLE_EXCEPTIONS
#include "CL/cl.hpp"


struct sz_opencl_state
{
  struct error_t
  {
    cl_int code = 0;
    const char* str = nullptr;
  } error;
  cl::Platform platform;
  cl::Device device;
  cl::Context context;
  cl::CommandQueue queue;
};


struct sz_opencl_sizes {
    sz_opencl_sizes(int block_size, int r1, int r2, int r3):
            r1(r1),
            r2(r2),
            r3(r3),
            block_size(block_size),
            num_x((r1 - 1) / block_size + 1),
            num_y((r2 - 1) / block_size + 1),
            num_z((r3 - 1) / block_size + 1),
            max_num_block_elements(block_size * block_size * block_size),
            num_blocks(num_x * num_y * num_z),
            num_elements( r1 * r2 * r3),
            dim0_offset(r2 * r3),
            dim1_offset(r3),
            params_offset_b(num_blocks),
            params_offset_c(2*num_blocks),
            params_offset_d(3*num_blocks),
            pred_buffer_block_size(  block_size + 1),
            strip_dim0_offset(  pred_buffer_block_size * pred_buffer_block_size),
            strip_dim1_offset(  pred_buffer_block_size),
            unpred_data_max_size(max_num_block_elements)
    {}

    const size_t r1,r2,r3;
    const size_t block_size;
    const size_t num_x;
    const size_t num_y;
    const size_t num_z;
    const size_t max_num_block_elements;
    const size_t num_blocks;
    const size_t num_elements;
    const size_t dim0_offset;
    const size_t dim1_offset;
    const size_t params_offset_b;
    const size_t params_offset_c;
    const size_t params_offset_d;
    const int pred_buffer_block_size;
    const int strip_dim0_offset;
    const int strip_dim1_offset;
    const size_t unpred_data_max_size;
};

float compute_mean(const float *oriData, double realPrecision, float dense_pos, size_t num_elements) {
	float mean = 0.0f;
	{
		// compute mean
		double sum = 0.0;
		size_t mean_count = 0;
		for(size_t i=0; i<num_elements; i++){
			if(fabs(oriData[i] - dense_pos) < realPrecision){
				sum += oriData[i];
				mean_count ++;
			}
		}
		if(mean_count > 0) mean = sum / mean_count;
	}
	return mean;
}

unsigned char *
encode_all_blocks(sz_opencl_sizes const *sizes, int *result_type, int *type, HuffmanTree *huffmanTree,
                  unsigned char *result_pos) {
    type = result_type;
    size_t total_type_array_size = 0;
    unsigned char * type_array_buffer = (unsigned char *) malloc(sizes->num_blocks*sizes->max_num_block_elements*sizeof(int));
    unsigned short * type_array_block_size = (unsigned short *) malloc(sizes->num_blocks*sizeof(unsigned short));
    unsigned char * type_array_buffer_pos = type_array_buffer;
    unsigned short * type_array_block_size_pos = type_array_block_size;
    for(size_t i=0; i<sizes->num_x; i++){
        for(size_t j=0; j<sizes->num_y; j++){
            for(size_t k=0; k<sizes->num_z; k++){
                size_t typeArray_size = 0;
                encode(huffmanTree, type, sizes->max_num_block_elements, type_array_buffer_pos, &typeArray_size);
                total_type_array_size += typeArray_size;
                *type_array_block_size_pos = typeArray_size;
                type_array_buffer_pos += typeArray_size;
                type += sizes->max_num_block_elements;
                type_array_block_size_pos ++;
            }
        }
    }
    size_t compressed_type_array_block_size;
    unsigned char * compressed_type_array_block = SZ_compress_args(SZ_UINT16, type_array_block_size, &compressed_type_array_block_size, ABS, 0.5, 0, 0, 0, 0, 0, 0, sizes->num_blocks);
    memcpy(result_pos, &compressed_type_array_block_size, sizeof(size_t));
    result_pos += sizeof(size_t);
    memcpy(result_pos, compressed_type_array_block, compressed_type_array_block_size);
    result_pos += compressed_type_array_block_size;
    memcpy(result_pos, type_array_buffer, total_type_array_size);
    result_pos += total_type_array_size;

    free(compressed_type_array_block);
    free(type_array_buffer);
    free(type_array_block_size);
    return result_pos;
}

void calculate_regression_coefficents(const float *oriData, sz_opencl_sizes const *sizes, float *reg_params_pos,
                                      float *pred_buffer, const float *data_pos, float *pred_buffer_pos) {
	for(size_t i=0; i < sizes->num_x; i++){
		for(size_t j=0; j<sizes->num_y; j++){
			for(size_t k=0; k<sizes->num_z; k++){
				data_pos = oriData + i*sizes->block_size * sizes->dim0_offset + j*sizes->block_size * sizes->dim1_offset + k*sizes->block_size;
				pred_buffer_pos = pred_buffer;
				for(size_t ii=0; ii<sizes->block_size; ii++){
					for(size_t jj=0; jj<sizes->block_size; jj++){
						for(size_t kk=0; kk<sizes->block_size; kk++){
							int ii_ = (i*sizes->block_size + ii < sizes->r1) ? ii : sizes->r1 - 1 - i*sizes->block_size;
							int jj_ = (j*sizes->block_size + jj < sizes->r2) ? jj : sizes->r2 - 1 - j*sizes->block_size;
							int kk_ = (k*sizes->block_size + kk < sizes->r3) ? kk : sizes->r3 - 1 - k*sizes->block_size;
							*pred_buffer_pos = *(data_pos + ii_*sizes->dim0_offset + jj_*sizes->dim1_offset + kk_);
							pred_buffer_pos ++;
						}
					}
				}

                /*Calculate regression coefficients*/
				{
					float * cur_data_pos = pred_buffer;
					float fx = 0.0;
					float fy = 0.0;
					float fz = 0.0;
					float f = 0;
					float sum_x, sum_y;
					float curData;
					for(size_t i=0; i<sizes->block_size; i++){
						sum_x = 0;
						for(size_t j=0; j<sizes->block_size; j++){
							sum_y = 0;
							for(size_t k=0; k<sizes->block_size; k++){
								curData = *cur_data_pos;
								sum_y += curData;
								fz += curData * k;
								cur_data_pos ++;
							}
							fy += sum_y * j;
							sum_x += sum_y;
						}
						fx += sum_x * i;
						f += sum_x;
					}
					float coeff = 1.0 / (sizes->block_size * sizes->block_size * sizes->block_size);
					reg_params_pos[0] = (2 * fx / (sizes->block_size - 1) - f) * 6 * coeff / (sizes->block_size + 1);
					reg_params_pos[sizes->params_offset_b] = (2 * fy / (sizes->block_size - 1) - f) * 6 * coeff / (sizes->block_size + 1);
					reg_params_pos[sizes->params_offset_c] = (2 * fz / (sizes->block_size - 1) - f) * 6 * coeff / (sizes->block_size + 1);
					reg_params_pos[sizes->params_offset_d] = f * coeff - ((sizes->block_size - 1) * reg_params_pos[0] / 2 + (sizes->block_size - 1) * reg_params_pos[sizes->params_offset_b] / 2 + (sizes->block_size - 1) * reg_params_pos[sizes->params_offset_c] / 2);
				}
				reg_params_pos ++;
			}
		}
	}
}

size_t
save_unpredictable_data(const float *data_pos, sz_opencl_sizes const *sizes, double realPrecision, float mean,
                        int intvCapacity, int intvRadius, bool use_mean, float *oriData, int *type,
                        float *unpredictable_data, int *result_type, float *reg_params_pos, float *pred_buffer,
                        float *pred_buffer_pos, const unsigned char *indicator_pos, int *blockwise_unpred_count_pos) {
	size_t total_unpred = 0;
        int intvCapacity_sz = intvCapacity - 2;
        type = result_type;
        for(size_t i=0; i<sizes->num_x; i++){
            for(size_t j=0; j<sizes->num_y; j++){
                for(size_t k=0; k<sizes->num_z; k++){
                    data_pos = oriData + i*sizes->block_size * sizes->dim0_offset + j*sizes->block_size * sizes->dim1_offset + k*sizes->block_size;
                    pred_buffer_pos = pred_buffer + sizes->pred_buffer_block_size*sizes->pred_buffer_block_size + sizes->pred_buffer_block_size + 1;
                    for(size_t ii=0; ii<sizes->block_size; ii++){
                        for(size_t jj=0; jj<sizes->block_size; jj++){
                            for(size_t kk=0; kk<sizes->block_size; kk++){
                                int ii_ = (i*sizes->block_size + ii < sizes->r1) ? ii : sizes->r1 - 1 - i*sizes->block_size;
                                int jj_ = (j*sizes->block_size + jj < sizes->r2) ? jj : sizes->r2 - 1 - j*sizes->block_size;
                                int kk_ = (k*sizes->block_size + kk < sizes->r3) ? kk : sizes->r3 - 1 - k*sizes->block_size;
                                *pred_buffer_pos = *(data_pos + ii_*sizes->dim0_offset + jj_*sizes->dim1_offset + kk_);
                                pred_buffer_pos ++;
                            }
                            pred_buffer_pos ++;
                        }
                        pred_buffer_pos += sizes->pred_buffer_block_size;
                    }

                    if(!(indicator_pos[k])){
                        float curData;
                        float pred;
                        double itvNum;
                        double diff;
                        size_t index = 0;
                        size_t block_unpredictable_count = 0;
                        float * cur_data_pos = pred_buffer + sizes->pred_buffer_block_size*sizes->pred_buffer_block_size + sizes->pred_buffer_block_size + 1;
                        for(size_t ii=0; ii<sizes->block_size; ii++){
                            for(size_t jj=0; jj<sizes->block_size; jj++){
                                for(size_t kk=0; kk<sizes->block_size; kk++){
                                    curData = *cur_data_pos;
                                    pred = reg_params_pos[0] * ii + reg_params_pos[sizes->params_offset_b] * jj + reg_params_pos[sizes->params_offset_c] * kk + reg_params_pos[sizes->params_offset_d];
                                    diff = curData - pred;
                                    itvNum = fabs(diff)/realPrecision + 1;
                                    if (itvNum < intvCapacity){
                                        if (diff < 0) itvNum = -itvNum;
                                        type[index] = (int) (itvNum/2) + intvRadius;
                                        pred = pred + 2 * (type[index] - intvRadius) * realPrecision;
                                        //ganrantee comporession error against the case of machine-epsilon
                                        if(fabs(curData - pred)>realPrecision){
                                            type[index] = 0;
                                            unpredictable_data[block_unpredictable_count ++] = curData;
                                        }
                                    }
                                    else{
                                        type[index] = 0;
                                        unpredictable_data[block_unpredictable_count ++] = curData;
                                    }
                                    index ++;
                                    cur_data_pos ++;
                                }
                                cur_data_pos ++;
                            }
                            cur_data_pos += sizes->pred_buffer_block_size;
                        }
                        reg_params_pos ++;
                        total_unpred += block_unpredictable_count;
                        unpredictable_data += block_unpredictable_count;
                        *blockwise_unpred_count_pos = block_unpredictable_count;
                    }
                    else{
// use SZ
// SZ predication
                        size_t unpredictable_count = 0;
                        float * cur_data_pos = pred_buffer + sizes->pred_buffer_block_size*sizes->pred_buffer_block_size + sizes->pred_buffer_block_size + 1;
                        float curData;
                        float pred3D;
                        double itvNum, diff;
                        size_t index = 0;
                        for(size_t ii=0; ii<sizes->block_size; ii++){
                            for(size_t jj=0; jj<sizes->block_size; jj++){
                                for(size_t kk=0; kk<sizes->block_size; kk++){

                                    curData = *cur_data_pos;
                                    if(use_mean && fabs(curData - mean) <= realPrecision){
                                        type[index] = 1;
                                        *cur_data_pos = mean;
                                    }
                                    else
                                    {
                                        pred3D = cur_data_pos[-1] + cur_data_pos[-sizes->strip_dim1_offset]+ cur_data_pos[-sizes->strip_dim0_offset] - cur_data_pos[-sizes->strip_dim1_offset - 1]
                                                 - cur_data_pos[-sizes->strip_dim0_offset - 1] - cur_data_pos[-sizes->strip_dim0_offset - sizes->strip_dim1_offset] + cur_data_pos[-sizes->strip_dim0_offset - sizes->strip_dim1_offset - 1];
                                        diff = curData - pred3D;
                                        itvNum = fabs(diff)/realPrecision + 1;
                                        if (itvNum < intvCapacity_sz){
                                            if (diff < 0) itvNum = -itvNum;
                                            type[index] = (int) (itvNum/2) + intvRadius;
                                            *cur_data_pos = pred3D + 2 * (type[index] - intvRadius) * realPrecision;
                                            //ganrantee comporession error against the case of machine-epsilon
                                            if(fabs(curData - *cur_data_pos)>realPrecision){
                                                type[index] = 0;
                                                *cur_data_pos = curData;
                                                unpredictable_data[unpredictable_count ++] = curData;
                                            }
                                        }
                                        else{
                                            type[index] = 0;
                                            *cur_data_pos = curData;
                                            unpredictable_data[unpredictable_count ++] = curData;
                                        }
                                    }
                                    index ++;
                                    cur_data_pos ++;
                                }
                                cur_data_pos ++;
                            }
                            cur_data_pos += sizes->pred_buffer_block_size;
                        }
                        total_unpred += unpredictable_count;
                        unpredictable_data += unpredictable_count;
                        *blockwise_unpred_count_pos = unpredictable_count;
                    }// end SZ
                    blockwise_unpred_count_pos ++;
                    type += sizes->block_size * sizes->block_size * sizes->block_size;
                } // end k
                indicator_pos += sizes->num_z;
            }// end j
        }// end i
    return total_unpred;
}

void compute_errors(const float *reg_params_pos, const float *pred_buffer, sz_opencl_sizes const *sizes, float mean,
                    float noise, bool use_mean, size_t i, size_t a, size_t b, size_t c, size_t d, float &err_sz,
                    float &err_reg) {
    const float* cur_data_pos = pred_buffer + i * sizes->pred_buffer_block_size * sizes->pred_buffer_block_size + a * sizes->pred_buffer_block_size + b;
    float curData = *cur_data_pos;
    float pred_sz = cur_data_pos[-1] + cur_data_pos[-sizes->strip_dim1_offset]+ cur_data_pos[-sizes->strip_dim0_offset] - cur_data_pos[-sizes->strip_dim1_offset - 1] - cur_data_pos[-sizes->strip_dim0_offset - 1] - cur_data_pos[-sizes->strip_dim0_offset - sizes->strip_dim1_offset] + cur_data_pos[-sizes->strip_dim0_offset - sizes->strip_dim1_offset - 1];
    float pred_reg = reg_params_pos[0] * (i-1) + reg_params_pos[sizes->params_offset_b] * c + reg_params_pos[sizes->params_offset_c] * d + reg_params_pos[sizes->params_offset_d];
    if(use_mean) {
        err_sz += std::min(fabs(pred_sz - curData) + noise, fabs(mean - curData));
        err_reg += fabs(pred_reg - curData);
    } else {
        err_sz += fabs(pred_sz - curData) + noise;
        err_reg += fabs(pred_reg - curData);
    }
}

void sz_opencl_sample(const sz_opencl_sizes *sizes, const float *oriData, const float *data_pos, float mean, float noise,
                      bool use_mean, float *pred_buffer, float *reg_params_pos, float *pred_buffer_pos,
                      unsigned char *indicator_pos) {
    for(size_t i=0; i<sizes->num_x; i++){
        for(size_t j=0; j<sizes->num_y; j++){
            for(size_t k=0; k<sizes->num_z; k++){
                data_pos = oriData + i*sizes->block_size * sizes->dim0_offset + j*sizes->block_size * sizes->dim1_offset + k*sizes->block_size;
// add 1 in x, y, z offset
                pred_buffer_pos = pred_buffer + sizes->pred_buffer_block_size*sizes->pred_buffer_block_size + sizes->pred_buffer_block_size + 1;
                for(size_t ii=0; ii<sizes->block_size; ii++){
                    for(size_t jj=0; jj<sizes->block_size; jj++){
                        for(size_t kk=0; kk<sizes->block_size; kk++){
                            int ii_ = (i*sizes->block_size + ii < sizes->r1) ? ii : sizes->r1 - 1 - i*sizes->block_size;
                            int jj_ = (j*sizes->block_size + jj < sizes->r2) ? jj : sizes->r2 - 1 - j*sizes->block_size;
                            int kk_ = (k*sizes->block_size + kk < sizes->r3) ? kk : sizes->r3 - 1 - k*sizes->block_size;
                            *pred_buffer_pos = *(data_pos + ii_*sizes->dim0_offset + jj_*sizes->dim1_offset + kk_);
                            pred_buffer_pos ++;
                        }
                        pred_buffer_pos ++;
                    }
                    // add 1 in y offset
                    pred_buffer_pos += sizes->pred_buffer_block_size;
                }
/*sampling and decide which predictor*/
                {
                    // sample point [1, 1, 1] [1, 1, 4] [1, 4, 1] [1, 4, 4] [4, 1, 1] [4, 1, 4] [4, 4, 1] [4, 4, 4]
                    float err_sz = 0.0, err_reg = 0.0;
                    for(size_t i=2; i<=sizes->block_size; i++){
                        compute_errors(reg_params_pos, pred_buffer, sizes,
                                       mean, noise, use_mean, i, i, i,
                                       (i - 1), (i - 1),
                                       err_sz,
                                       err_reg);

                        int bmi = sizes->block_size - i + 1;
                        compute_errors(reg_params_pos, pred_buffer, sizes,
                                       mean, noise, use_mean, i, i, bmi,
                                       (i - 1), bmi,
                                       err_sz,
                                       err_reg);


                        compute_errors(reg_params_pos, pred_buffer, sizes,
                                       mean, noise, use_mean, i, bmi, i,
                                       bmi, (i - 1),
                                       err_sz,
                                       err_reg);

                        compute_errors(reg_params_pos, pred_buffer, sizes,
                                       mean, noise, use_mean, i, bmi, bmi,
                                       bmi, bmi,
                                       err_sz,
                                       err_reg);
                    }
                    // indicator_pos[k] = (err_sz < err_reg);
                    indicator_pos[k] = err_reg >= err_sz;
                }
                reg_params_pos ++;
            } // end k
            indicator_pos += sizes->num_z;
        }// end j
    }// end i
}


extern "C"
{
  int sz_opencl_init(struct sz_opencl_state** state)
  {
    try {
      *state = new sz_opencl_state;

      std::vector<cl::Platform> platforms;
      cl::Platform::get(&platforms);

      auto valid_platform =
        std::find_if(std::begin(platforms), std::end(platforms),
                     [state](cl::Platform const& platform) {
                       try {
                         std::vector<cl::Device> devices;
                         platform.getDevices(CL_DEVICE_TYPE_GPU, &devices);
                         (*state)->device = devices.front();
                         (*state)->platform = platform;
                         return true;
                       } catch (cl::Error const& error) {
                         if (error.err() != CL_DEVICE_NOT_FOUND)
                           throw;
                       }
                       return false;
                     });
      if (valid_platform == std::end(platforms))
        throw cl::Error(CL_DEVICE_NOT_FOUND, "Failed to find a GPU");

      (*state)->context = cl::Context({ (*state)->device });
      (*state)->queue = cl::CommandQueue((*state)->context, (*state)->device);

      return SZ_SCES;
    } catch (cl::Error const& cl_error) {
      (*state)->error.code = cl_error.err();
      (*state)->error.str = cl_error.what();
      return SZ_NSCS;

    } catch (...) {
      delete *state;
      *state = nullptr;
      return SZ_NSCS;
    }
  }

  int sz_opencl_release(struct sz_opencl_state** state)
  {
    delete *state;

    return SZ_SCES;
  }

  const char* sz_opencl_error_msg(struct sz_opencl_state* state)
  {
    if (state == nullptr) {
      return "sz opencl allocation failed";
    }

    return state->error.str;
  }

  int sz_opencl_error_code(struct sz_opencl_state* state)
  {
    if (state == nullptr) {
      return -1;
    }

    return state->error.code;
  }

  int sz_opencl_check(struct sz_opencl_state* state)
  {
    try {
      std::string vec_add(
        R"(
				kernel void add(__global float* a, __global float* b, __global float* c)
				{
					int id = get_global_id(0);
					c[id] = a[id] + b[id];
				}
				)");
      cl::Program::Sources sources(
        1, std::make_pair(vec_add.c_str(), vec_add.size() + 1));

      cl::Program program(state->context, sources);
      program.build({ state->device });
      cl::Kernel kernel(program, "add");
      const int size = 1024;
      std::vector<float> h_a(size);
      std::vector<float> h_b(size);
      std::vector<float> h_c(size);
      std::vector<float> verify(size);
      cl::Buffer d_a(state->context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                     sizeof(cl_float) * size);
      cl::Buffer d_b(state->context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                     sizeof(cl_float) * size);
      cl::Buffer d_c(state->context, CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR,
                     sizeof(cl_float) * size);

      auto random_fill = [](std::vector<float>& vec, int seed) {
        std::seed_seq seed_seq{ seed };
        std::mt19937 gen(seed_seq);
        std::uniform_real_distribution<float> dist;

        std::generate(std::begin(vec), std::end(vec),
                      [&dist, &gen]() { return dist(gen); });
      };
      random_fill(h_a, 0);
      random_fill(h_b, 1);
      random_fill(h_c, 2);
      std::transform(std::begin(h_a), std::end(h_a), std::begin(h_b),
                     std::begin(verify),
                     [](float a, float b) { return a + b; });

      kernel.setArg(0, d_a);
      kernel.setArg(1, d_b);
      kernel.setArg(2, d_c);

      state->queue.enqueueWriteBuffer(d_a, /*blocking*/ CL_TRUE, /*offset*/ 0,
                                      /*size*/ sizeof(cl_float) * size,
                                      h_a.data());
      state->queue.enqueueWriteBuffer(d_b, /*blocking*/ CL_TRUE, /*offset*/ 0,
                                      /*size*/ sizeof(cl_float) * size,
                                      h_b.data());
      state->queue.enqueueNDRangeKernel(kernel, /*offset*/ cl::NullRange,
                                        /*global*/ cl::NDRange(size),
                                        cl::NullRange);
      state->queue.finish();
      state->queue.enqueueReadBuffer(d_c, /*blocking*/ CL_TRUE, /*offset*/ 0,
                                     /*size*/ sizeof(cl_float) * size,
                                     h_c.data());

      if (std::equal(std::begin(h_c), std::end(h_c), std::begin(verify),
                     std::end(verify))) {
        return SZ_SCES;
      } else {
        return SZ_NSCS;
      }
    } catch (cl::Error const& error) {
      state->error.code = error.err();
      state->error.str = error.what();
      return SZ_NSCS;
    }
  }

unsigned char * sz_compress_float3d_opencl(float *oriData, size_t r1, size_t r2, size_t r3, double realPrecision, size_t * comp_size){
	unsigned int quantization_intervals;
	bool use_mean = false;

	// calculate block dims
	const sz_opencl_sizes sizes (/*block_size*/6, r1, r2, r3);


	int * result_type = (int *) malloc(sizes.num_blocks*sizes.max_num_block_elements * sizeof(int));
	float * result_unpredictable_data = (float *) malloc(sizes.unpred_data_max_size * sizeof(float) * sizes.num_blocks);
	float * reg_params = (float *) malloc(sizes.num_blocks * 4 * sizeof(float));
	float * pred_buffer = (float *) malloc((sizes.block_size+1)*(sizes.block_size+1)*(sizes.block_size+1)*sizeof(float));
	unsigned char * indicator = (unsigned char *) calloc(sizes.num_blocks,sizeof(unsigned char));
	int * blockwise_unpred_count = (int *) malloc(sizes.num_blocks * sizeof(int));

	float * data_pos = oriData;
	int * type = result_type;
	float * reg_params_pos = reg_params;
	float * pred_buffer_pos = NULL;
	int * blockwise_unpred_count_pos = blockwise_unpred_count;

    calculate_regression_coefficents(oriData, &sizes,
                                     reg_params_pos, pred_buffer, data_pos,
                                     pred_buffer_pos);

    float mean = 0.0f;
    if(exe_params->optQuantMode==1)
	{
		float mean_flush_freq, dense_pos, sz_sample_correct_freq = -1;//0.5; //-1
		quantization_intervals = optimize_intervals_float_3D_with_freq_and_dense_pos(oriData, r1, r2, r3, realPrecision, &dense_pos, &sz_sample_correct_freq, &mean_flush_freq);
		if(mean_flush_freq > 0.5 || mean_flush_freq > sz_sample_correct_freq)
		{
			use_mean = 1;
			mean = compute_mean(oriData, realPrecision, dense_pos, sizes.num_elements);
		}
		updateQuantizationInfo(quantization_intervals);
	} else{
		quantization_intervals = exe_params->intvCapacity;
	}



	// use two prediction buffers for higher performance
	float * unpredictable_data = result_unpredictable_data;
	unsigned char * indicator_pos = indicator;

	int intvCapacity = exe_params->intvCapacity;
	int intvRadius = exe_params->intvRadius;
	float noise = realPrecision * 1.22;
	reg_params_pos = reg_params;

	memset(pred_buffer, 0, (sizes.block_size+1)*(sizes.block_size+1)*(sizes.block_size+1)*sizeof(float));

	// select
    sz_opencl_sample(&sizes, oriData, data_pos, mean, noise, use_mean, pred_buffer,
                     reg_params_pos, pred_buffer_pos,
                     indicator_pos);

	size_t reg_count = 0;
	for(size_t i=0; i<sizes.num_blocks; i++){
		if(!(indicator[i])){
			reg_params[reg_count] = reg_params[i];
			reg_params[reg_count + sizes.params_offset_b] = reg_params[i + sizes.params_offset_b];
			reg_params[reg_count + sizes.params_offset_c] = reg_params[i + sizes.params_offset_c];
			reg_params[reg_count + sizes.params_offset_d] = reg_params[i + sizes.params_offset_d];
			reg_count ++;
		}
	}

	//Compress coefficient arrays
	const float rel_param_err = 0.025;
    const int coeff_intvCapacity_sz = 65536;
    const int coeff_intvRadius = coeff_intvCapacity_sz / 2;
    const double precision[4] = { rel_param_err * realPrecision / sizes.block_size,
                                  rel_param_err * realPrecision / sizes.block_size,
                                  rel_param_err * realPrecision / sizes.block_size,
                                  rel_param_err * realPrecision};

	float last_coeffcients[4] = {0.0};
	int * coeff_type[4];
	int * coeff_result_type = (int *) malloc(reg_count*4*sizeof(int));
	float * coeff_unpred_data[4];
	float * coeff_unpredictable_data = (float *) malloc(reg_count*4*sizeof(float));
    unsigned int coeff_unpredictable_count[4] = {0};

	for(int i=0; i<4; i++){
		coeff_type[i] = coeff_result_type + i * reg_count;
		coeff_unpred_data[i] = coeff_unpredictable_data + i * reg_count;
	}

	float * reg_params_separte[4];
	for(int i=0; i<4; i++){
		reg_params_separte[i] = reg_params + i * sizes.num_blocks;
	}

	for(size_t coeff_index=0; coeff_index<reg_count;  coeff_index++){
		for(int e=0; e<4; e++){
			const float cur_coeff = reg_params_separte[e][coeff_index];
			const double diff = cur_coeff - last_coeffcients[e];
			double itvNum = fabs(diff)/precision[e] + 1;
			if (itvNum < coeff_intvCapacity_sz){
				if (diff < 0) itvNum = -itvNum;
				coeff_type[e][coeff_index] = (int) (itvNum/2) + coeff_intvRadius;
				last_coeffcients[e] = last_coeffcients[e] + 2 * (coeff_type[e][coeff_index] - coeff_intvRadius) * precision[e];
				//ganrantee compression error against the case of machine-epsilon
				if(fabs(cur_coeff - last_coeffcients[e])>precision[e]){
					coeff_type[e][coeff_index] = 0;
					last_coeffcients[e] = cur_coeff;
					coeff_unpred_data[e][coeff_unpredictable_count[e] ++] = cur_coeff;
				}
			}
			else{
				coeff_type[e][coeff_index] = 0;
				last_coeffcients[e] = cur_coeff;
				coeff_unpred_data[e][coeff_unpredictable_count[e] ++] = cur_coeff;
			}
			reg_params_separte[e][coeff_index] = last_coeffcients[e];
		}
	}

	// pred & quantization
	reg_params_pos = reg_params;
	indicator_pos = indicator;

    size_t total_unpred = save_unpredictable_data(data_pos, &sizes, realPrecision, mean, intvCapacity, intvRadius,
                                                  use_mean, oriData, type,
                                                  unpredictable_data,
                                                  result_type,
                                                  reg_params_pos,
                                                  pred_buffer,
                                                  pred_buffer_pos, indicator_pos, blockwise_unpred_count_pos);

	free(pred_buffer);
	int stateNum = 2*quantization_intervals;
	HuffmanTree* huffmanTree = createHuffmanTree(stateNum);

	size_t nodeCount = 0;
	init(huffmanTree, result_type, sizes.num_blocks*sizes.max_num_block_elements);
	size_t i = 0;
	for (i = 0; i < huffmanTree->stateNum; i++)
		if (huffmanTree->code[i]) nodeCount++;
	nodeCount = nodeCount*2-1;

	unsigned char *treeBytes;
	unsigned int treeByteSize = convert_HuffTree_to_bytes_anyStates(huffmanTree, nodeCount, &treeBytes);

	const unsigned int meta_data_offset = 3 + 1 + MetaDataByteLength;
	// total size 										metadata		  # elements     real precision		intervals	nodeCount		huffman 	 	block index 						unpredicatable count						mean 					 	unpred size 				elements
	unsigned char * result = (unsigned char *) calloc(meta_data_offset + exe_params->SZ_SIZE_TYPE + sizeof(double) + sizeof(int) + sizeof(int) + treeByteSize + sizes.num_blocks * sizeof(unsigned short) + sizes.num_blocks * sizeof(unsigned short) + sizes.num_blocks * sizeof(float) + total_unpred * sizeof(float) + sizes.num_elements * sizeof(int), 1);
	unsigned char * result_pos = result;
	initRandomAccessBytes(result_pos);

	result_pos += meta_data_offset;

	sizeToBytes(result_pos,sizes.num_elements); //SZ_SIZE_TYPE: 4 or 8
	result_pos += exe_params->SZ_SIZE_TYPE;

	intToBytes_bigEndian(result_pos, sizes.block_size);
	result_pos += sizeof(int);
	doubleToBytes(result_pos, realPrecision);
	result_pos += sizeof(double);
	intToBytes_bigEndian(result_pos, quantization_intervals);
	result_pos += sizeof(int);
	intToBytes_bigEndian(result_pos, treeByteSize);
	result_pos += sizeof(int);
	intToBytes_bigEndian(result_pos, nodeCount);
	result_pos += sizeof(int);
	memcpy(result_pos, treeBytes, treeByteSize);
	result_pos += treeByteSize;
	free(treeBytes);

	memcpy(result_pos, &use_mean, sizeof(unsigned char));
	result_pos += sizeof(unsigned char);
	memcpy(result_pos, &mean, sizeof(float));
	result_pos += sizeof(float);
	size_t indicator_size = convertIntArray2ByteArray_fast_1b_to_result(indicator, sizes.num_blocks, result_pos);
	result_pos += indicator_size;

	//convert the lead/mid/resi to byte stream
	if(reg_count > 0){
		for(int e=0; e<4; e++){
			int stateNum = 2*coeff_intvCapacity_sz;
			HuffmanTree* huffmanTree = createHuffmanTree(stateNum);
			size_t nodeCount = 0;
			init(huffmanTree, coeff_type[e], reg_count);
			size_t i = 0;
			for (i = 0; i < huffmanTree->stateNum; i++)
				if (huffmanTree->code[i]) nodeCount++;
			nodeCount = nodeCount*2-1;
			unsigned char *treeBytes;
			unsigned int treeByteSize = convert_HuffTree_to_bytes_anyStates(huffmanTree, nodeCount, &treeBytes);
			doubleToBytes(result_pos, precision[e]);
			result_pos += sizeof(double);
			intToBytes_bigEndian(result_pos, coeff_intvRadius);
			result_pos += sizeof(int);
			intToBytes_bigEndian(result_pos, treeByteSize);
			result_pos += sizeof(int);
			intToBytes_bigEndian(result_pos, nodeCount);
			result_pos += sizeof(int);
			memcpy(result_pos, treeBytes, treeByteSize);
			result_pos += treeByteSize;
			free(treeBytes);
			size_t typeArray_size = 0;
			encode(huffmanTree, coeff_type[e], reg_count, result_pos + sizeof(size_t), &typeArray_size);
			sizeToBytes(result_pos, typeArray_size);
			result_pos += sizeof(size_t) + typeArray_size;
			intToBytes_bigEndian(result_pos, coeff_unpredictable_count[e]);
			result_pos += sizeof(int);
			memcpy(result_pos, coeff_unpred_data[e], coeff_unpredictable_count[e]*sizeof(float));
			result_pos += coeff_unpredictable_count[e]*sizeof(float);
			SZ_ReleaseHuffman(huffmanTree);
		}
	}
	free(coeff_result_type);
	free(coeff_unpredictable_data);

	//record the number of unpredictable data and also store them
	memcpy(result_pos, &total_unpred, sizeof(size_t));
	result_pos += sizeof(size_t);
	// record blockwise unpred data
	size_t compressed_blockwise_unpred_count_size;
	unsigned char * compressed_bw_unpred_count = SZ_compress_args(SZ_INT32, blockwise_unpred_count, &compressed_blockwise_unpred_count_size, ABS, 0.5, 0, 0, 0, 0, 0, 0, sizes.num_blocks);
	memcpy(result_pos, &compressed_blockwise_unpred_count_size, sizeof(size_t));
	result_pos += sizeof(size_t);
	memcpy(result_pos, compressed_bw_unpred_count, compressed_blockwise_unpred_count_size);
	result_pos += compressed_blockwise_unpred_count_size;
	free(blockwise_unpred_count);
	free(compressed_bw_unpred_count);
	memcpy(result_pos, result_unpredictable_data, total_unpred * sizeof(float));
	result_pos += total_unpred * sizeof(float);

	free(reg_params);
	free(indicator);
	free(result_unpredictable_data);
	// encode type array by block

	result_pos = encode_all_blocks(&sizes, result_type, type,
                                   huffmanTree,
                                   result_pos);

	size_t totalEncodeSize = result_pos - result;

	free(result_type);
	SZ_ReleaseHuffman(huffmanTree);
	*comp_size = totalEncodeSize;
	return result;
}

int sz_decompress_float_opencl(float** newData,
							   size_t r5, size_t r4, size_t r3, size_t r2, size_t r1,
							   size_t s5, size_t s4, size_t s3, size_t s2, size_t s1, // start point
							   size_t e5, size_t e4, size_t e3, size_t e2, size_t e1, // end point
							   unsigned char* cmpBytes, size_t cmpSize)
{
	if(confparams_dec==NULL)
		confparams_dec = (sz_params*)malloc(sizeof(sz_params));
	memset(confparams_dec, 0, sizeof(sz_params));
	if(exe_params==NULL)
		exe_params = (sz_exedata*)malloc(sizeof(sz_exedata));
	memset(exe_params, 0, sizeof(sz_exedata));

	int x = 1;
	char *y = (char*)&x;
	if(*y==1)
		sysEndianType = LITTLE_ENDIAN_SYSTEM;
	else //=0
		sysEndianType = BIG_ENDIAN_SYSTEM;

	confparams_dec->randomAccess = 1;

	int status = SZ_SCES;
	size_t dataLength = computeDataLength(r5,r4,r3,r2,r1);

	//unsigned char* tmpBytes;
	size_t targetUncompressSize = dataLength <<2; //i.e., *4
	//tmpSize must be "much" smaller than dataLength
	size_t i, tmpSize = 8+MetaDataByteLength+exe_params->SZ_SIZE_TYPE;
	unsigned char* szTmpBytes;

	if(cmpSize!=8+4+MetaDataByteLength && cmpSize!=8+8+MetaDataByteLength) //4,8 means two posibilities of SZ_SIZE_TYPE
	{
		confparams_dec->losslessCompressor = is_lossless_compressed_data(cmpBytes, cmpSize);
		if(confparams_dec->szMode!=SZ_TEMPORAL_COMPRESSION)
		{
			if(confparams_dec->losslessCompressor!=-1)
				confparams_dec->szMode = SZ_BEST_COMPRESSION;
			else
				confparams_dec->szMode = SZ_BEST_SPEED;
		}

		if(confparams_dec->szMode==SZ_BEST_SPEED)
		{
			tmpSize = cmpSize;
			szTmpBytes = cmpBytes;
		}
		else if(confparams_dec->szMode==SZ_BEST_COMPRESSION || confparams_dec->szMode==SZ_DEFAULT_COMPRESSION || confparams_dec->szMode==SZ_TEMPORAL_COMPRESSION)
		{
			if(targetUncompressSize<MIN_ZLIB_DEC_ALLOMEM_BYTES) //Considering the minimum size
				targetUncompressSize = MIN_ZLIB_DEC_ALLOMEM_BYTES;
			tmpSize = sz_lossless_decompress(confparams_dec->losslessCompressor, cmpBytes, (unsigned long)cmpSize, &szTmpBytes, (unsigned long)targetUncompressSize+4+MetaDataByteLength+exe_params->SZ_SIZE_TYPE);//		(unsigned long)targetUncompressSize+8: consider the total length under lossless compression mode is actually 3+4+1+targetUncompressSize
		}
		else
		{
			printf("Wrong value of confparams_dec->szMode in the double compressed bytes.\n");
			status = SZ_MERR;
			return status;
		}
	}
	else
		szTmpBytes = cmpBytes;

	TightDataPointStorageF* tdps;
	new_TightDataPointStorageF_fromFlatBytes(&tdps, szTmpBytes, tmpSize);

	int dim = computeDimension(r5,r4,r3,r2,r1);
	int floatSize = sizeof(float);
	if(tdps->isLossless)
	{
		*newData = (float*)malloc(floatSize*dataLength);
		if(sysEndianType==BIG_ENDIAN_SYSTEM)
		{
			memcpy(*newData, szTmpBytes+4+MetaDataByteLength+exe_params->SZ_SIZE_TYPE, dataLength*floatSize);
		}
		else
		{
			unsigned char* p = szTmpBytes+4+MetaDataByteLength+exe_params->SZ_SIZE_TYPE;
			for(i=0;i<dataLength;i++,p+=floatSize)
				(*newData)[i] = bytesToFloat(p);
		}
	}
	else
	{
		if(confparams_dec->randomAccess == 0 && (s1+s2+s3+s4+s5>0 || (r5-e5+r4-e4+r3-e3+r2-e2+r1-e1 > 0)))
		{
			printf("Error: you specified the random access mode for decompression, but the compressed data were generate in the non-random-access way.!\n");
			status = SZ_DERR;
		} else if (dim == 1) {
			printf("Error: random access mode doesn't support 1D yet, but only 3D.\n");
			status = SZ_DERR;
		} else if(dim == 2) {
			printf("Error: random access mode doesn't support 2D yet, but only 3D.\n");
			status = SZ_DERR;
		} else if(dim == 3) {
			decompressDataSeries_float_3D_decompression_given_areas_with_blocked_regression(newData, r3, r2, r1, s3, s2, s1, e3, e2, e1, tdps->raBytes);
			status = SZ_SCES;
		} else if(dim == 4) {
			printf("Error: random access mode doesn't support 4D yet, but only 3D.\n");
			status = SZ_DERR;
		} else {
			printf("Error: currently support only at most 4 dimensions!\n");
			status = SZ_DERR;
		}
	}

	free_TightDataPointStorageF2(tdps);
	if(confparams_dec->szMode!=SZ_BEST_SPEED && cmpSize!=8+MetaDataByteLength+exe_params->SZ_SIZE_TYPE)
		free(szTmpBytes);
	return status;
}

}
