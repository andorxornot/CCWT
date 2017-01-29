#include <ccwt.h>
#include <fftw3.h>
#include <pthread.h>

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

void ccwt_frequency_band(double* frequency_band, unsigned int frequency_band_count, double frequency_range, double frequency_offset, double frequency_basis, double deviation) {
    if(frequency_range == 0.0)
        frequency_range = frequency_band_count/2;
    for(unsigned int y = 0; y < frequency_band_count; ++y) {
        double frequency = frequency_range*(1.0-(double)y/frequency_band_count)+frequency_offset,
               frequency_derivative = frequency_range/frequency_band_count;
        if(frequency_basis > 0.0) {
            frequency = pow(frequency_basis, frequency);
            frequency_derivative *= log(frequency_basis)*frequency;
        }
        frequency_band[y*2  ] = frequency;
        frequency_band[y*2+1] = frequency_derivative*deviation;
    }
}

complex double* ccwt_fft(unsigned int input_width, unsigned int input_padding, unsigned int thread_count, void* input, unsigned char is_double_precision, unsigned char is_complex) {
    unsigned int input_sample_count = input_width+2*input_padding;
    complex double* output = (complex double*)fftw_malloc(sizeof(fftw_complex)*input_sample_count);
    if(!output)
        return NULL;

    for(unsigned int i = 0; i < input_padding; ++i)
        output[i] = output[input_sample_count-i-1] = 0;

    if(is_double_precision) {
        if(is_complex)
            for(unsigned int i = 0; i < input_width; ++i)
                output[input_padding+i] = ((complex double*)input)[i];
        else
            for(unsigned int i = 0; i < input_width; ++i)
                ((double*)output)[input_padding+i] = ((double*)input)[i];
    } else {
        if(is_complex)
            for(unsigned int i = 0; i < input_width; ++i)
                output[input_padding+i] = ((complex float*)input)[i];
        else
            for(unsigned int i = 0; i < input_width; ++i)
                ((double*)output)[input_padding+i] = ((float*)input)[i];
    }

    fftw_plan_with_nthreads(thread_count);
    fftw_plan input_plan = (is_complex)
        ? fftw_plan_dft_1d(input_sample_count, (fftw_complex*)output, (fftw_complex*)output, FFTW_FORWARD, FFTW_ESTIMATE)
        : fftw_plan_dft_r2c_1d(input_sample_count, (double*)output, (fftw_complex*)output, FFTW_ESTIMATE);
    if(!input_plan)
        goto cleanup;

    fftw_execute(input_plan);
    cleanup:
    fftw_destroy_plan(input_plan);
    return output;
}

void* ccwt_calculate_thread(void* ptr) {
    struct ccwt_thread_data* thread = (struct ccwt_thread_data*)ptr;
    struct ccwt_data* ccwt = thread->ccwt;
    complex double* output = thread->output;
    double scale_factor = 1.0/(double)ccwt->input_sample_count,
           padding_correction = (double)ccwt->input_sample_count/ccwt->input_width;

    for(unsigned int y = thread->begin_y; y < thread->end_y && !thread->return_value; ++y) {
        double frequency = ccwt->frequency_band[y*2  ]*padding_correction,
               deviation = ccwt->frequency_band[y*2+1]*ccwt->output_sample_count*padding_correction;
        gabor_wavelet(ccwt->input_sample_count, output, frequency, deviation);

        for(unsigned int i = 0; i < ccwt->output_sample_count; ++i)
            output[i] = output[i]*ccwt->input[i]*scale_factor;
        if(ccwt->output_sample_count < ccwt->input_sample_count) {
            unsigned int rest = ccwt->input_sample_count%ccwt->output_sample_count, cut_index = ccwt->input_sample_count-rest;
            for(unsigned int chunk_index = ccwt->output_sample_count; chunk_index < cut_index; chunk_index += ccwt->output_sample_count)
                for(unsigned int i = 0; i < ccwt->output_sample_count; ++i)
                    output[i] += output[chunk_index+i]*ccwt->input[chunk_index+i]*scale_factor;
            for(unsigned int i = 0; i < rest; ++i)
                output[i] += output[cut_index+i]*ccwt->input[cut_index+i]*scale_factor;
        }

        fftw_execute_dft((fftw_plan)ccwt->fftw_plan, (fftw_complex*)output, (fftw_complex*)output);
        thread->return_value = ccwt->callback(thread, y);
    }
    return NULL;
}

int ccwt_calculate(struct ccwt_data* ccwt) {
    ccwt->output_sample_count = ccwt->output_width*((double)ccwt->input_sample_count/ccwt->input_width);
    ccwt->output_padding = ccwt->input_padding*((double)ccwt->output_width/ccwt->input_width);
    if(ccwt->output_width > ccwt->input_width)
        return -2;
    if(ccwt->thread_count == 0)
        ccwt->thread_count = 1;

    int return_value;
    ccwt->threads = (struct ccwt_thread_data*)malloc(sizeof(struct ccwt_thread_data)*ccwt->thread_count);
    if(!ccwt->threads)
        goto cleanup;
    unsigned int slice_size = ccwt->height/ccwt->thread_count;
    ccwt->threads[0].pthread = NULL;
    ccwt->threads[0].begin_y = 0;
    ccwt->threads[0].end_y = ccwt->height-slice_size*(ccwt->thread_count-1);
    for(unsigned int t = 0; t < ccwt->thread_count; ++t) {
        ccwt->threads[t].return_value = 0;
        if(t > 0) {
            ccwt->threads[t].begin_y = ccwt->threads[t-1].end_y;
            ccwt->threads[t].end_y = ccwt->threads[t].begin_y+slice_size;
        }
        ccwt->threads[t].thread_index = t;
        ccwt->threads[t].output = (complex double*)fftw_malloc(sizeof(fftw_complex)*ccwt->input_sample_count);
        ccwt->threads[t].ccwt = ccwt;
        return_value = -1;
        if(!ccwt->threads[t].output)
            goto cleanup;
    }

    fftw_plan_with_nthreads(1);
    ccwt->fftw_plan = fftw_plan_dft_1d(ccwt->output_sample_count, (fftw_complex*)ccwt->threads[0].output, (fftw_complex*)ccwt->threads[0].output, FFTW_BACKWARD, FFTW_MEASURE);
    if(!ccwt->fftw_plan)
        goto cleanup;

    return_value = -4;
    for(unsigned int t = 1; t < ccwt->thread_count; ++t)
        if(pthread_create((pthread_t*)&ccwt->threads[t].pthread, NULL, ccwt_calculate_thread, &ccwt->threads[t]))
            goto cleanup;
    ccwt_calculate_thread(&ccwt->threads[0]);
    return_value = ccwt->threads[0].return_value;
    for(unsigned int t = 1; t < ccwt->thread_count; ++t) {
        pthread_join((pthread_t)ccwt->threads[t].pthread, NULL);
        if(!return_value && ccwt->threads[t].return_value)
            return_value = ccwt->threads[t].return_value;
    }

    fftw_destroy_plan(ccwt->fftw_plan);
    cleanup:
    for(unsigned int t = 0; t < ccwt->thread_count; ++t)
        fftw_free(ccwt->threads[t].output);
    free(ccwt->threads);
    return return_value;
}
