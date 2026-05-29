#ifndef OPERA_LR_DSP_H_INCLUDED
#define OPERA_LR_DSP_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void opera_lr_dsp_init(const bool threaded);
void opera_lr_dsp_destroy(void);

void opera_lr_dsp_upload(void);
void opera_lr_dsp_process(void);

void opera_lr_cdda_mix(int16_t *dsp_buf_, size_t frames_);

#endif