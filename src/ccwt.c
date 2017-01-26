#include <ccwt.h>
#include <fftw3.h>
#include <math.h>

void gabor_wavelet(unsigned int sample_count, complex double* kernel, double center_frequency, double deviation) {
    deviation = 1.0/sqrt(deviation);
    unsigned int half_sample_count = sample_count>>1;
    for(unsigned int i = 0; i < sample_count; ++i) {
        double f = fabs(i-center_frequency);
        f = half_sample_count-fabs(f-half_sample_count);
        f *= deviation;
        kernel[i] = exp(-f*f);
    }
}

void convolve_and_downsample(unsigned int dst_sample_count, unsigned int src_sample_count, complex double* dst, complex double* src) {
    double scaleFactor = 1.0/(double)src_sample_count;
    for(unsigned int i = 0; i < dst_sample_count; ++i)
        dst[i] = dst[i]*src[i]*scaleFactor;
    if(dst_sample_count >= src_sample_count)
        return;
    unsigned int rest = src_sample_count%dst_sample_count, cut_index = src_sample_count-rest;
    for(unsigned int chunk_index = dst_sample_count; chunk_index < cut_index; chunk_index += dst_sample_count)
        for(unsigned int i = 0; i < dst_sample_count; ++i)
            dst[i] += dst[chunk_index+i]*src[chunk_index+i]*scaleFactor;
    for(unsigned int i = 0; i < rest; ++i)
        dst[i] += dst[cut_index+i]*src[cut_index+i]*scaleFactor;
}

int ccwt_init(struct ccwt_data* ccwt) {
    ccwt->input_sample_count = ccwt->input_width+2*ccwt->input_padding;
    ccwt->padding_correction = (double)ccwt->input_sample_count/ccwt->input_width;
    ccwt->output_sample_count = ccwt->output_width*ccwt->padding_correction;
    ccwt->output_padding = ccwt->input_padding*(double)ccwt->output_width/ccwt->input_width;
    ccwt->input = (complex double*)fftw_malloc(sizeof(fftw_complex)*ccwt->input_sample_count);
    ccwt->output = (complex double*)fftw_malloc(sizeof(fftw_complex)*ccwt->input_sample_count);
    ccwt->input_plan = fftw_plan_dft_1d(ccwt->input_sample_count, (fftw_complex*)ccwt->input, (fftw_complex*)ccwt->input, FFTW_FORWARD, FFTW_ESTIMATE);
    ccwt->output_plan = fftw_plan_dft_1d(ccwt->output_sample_count, (fftw_complex*)ccwt->output, (fftw_complex*)ccwt->output, FFTW_BACKWARD, FFTW_MEASURE);
    if(!ccwt->input || !ccwt->output || !ccwt->input_plan || !ccwt->output_plan)
        return -1;
    for(unsigned int i = 0; i < ccwt->input_padding; ++i)
        ccwt->input[i] = ccwt->input[ccwt->input_sample_count-i-1] = 0;
    return 0;
}

int ccwt_cleanup(struct ccwt_data* ccwt) {
    fftw_free(ccwt->input);
    fftw_free(ccwt->output);
    fftw_destroy_plan(ccwt->input_plan);
    fftw_destroy_plan(ccwt->output_plan);
    return 0;
}

int ccwt_calculate(struct ccwt_data* ccwt, void* user_data, int(*callback)(struct ccwt_data*, void*, unsigned int)) {
    int return_value = 0;
    fftw_execute(ccwt->input_plan);
    for(unsigned int y = 0; y < ccwt->height && !return_value; ++y) {
        double frequency = ccwt->frequency_range*(1.0-(double)y/ccwt->height)+ccwt->frequency_offset,
               frequency_derivative = ccwt->frequency_range/ccwt->height;
        if(ccwt->frequency_basis > 0.0) {
            frequency = pow(ccwt->frequency_basis, frequency);
            frequency_derivative *= log(ccwt->frequency_basis)*frequency;
        }
        gabor_wavelet(
            ccwt->input_sample_count, ccwt->output,
            frequency*ccwt->padding_correction,
            ccwt->deviation*ccwt->output_sample_count*frequency_derivative*ccwt->padding_correction
        );
        convolve_and_downsample(ccwt->output_sample_count, ccwt->input_sample_count, ccwt->output, ccwt->input);
        fftw_execute(ccwt->output_plan);
        return_value = callback(ccwt, user_data, y);
    }
    return return_value;
}
