#ifndef OSCILL_SCOPE_VIEW_H
#define OSCILL_SCOPE_VIEW_H
#include "adc_capture.h"

#define SV_FRAME_PERIOD_TICK_MS 67U
#define SV_LOAD_DURATION_TICK_MS 63000U
typedef struct {
 uint32_t g2_complete,frames,frame_period_tick_ms,window_samples;
 uint32_t frame_generation,clear_pending,clear_start_tick_ms,render_max_cycles;
 uint32_t start_tick_ms,elapsed_tick_ms,progress_tick_ms,progress_blocks;
 uint64_t capture_work_cycles,graphics_work_cycles;
 uint32_t capture_call_max_cycles,graphics_call_max_cycles;
 uint32_t g2_frames,g2_elapsed_tick_ms;
 uint64_t g2_capture_work_cycles,g2_graphics_work_cycles;
 uint32_t g2_render_max_cycles,g2_capture_call_max_cycles,g2_graphics_call_max_cycles;
 uint32_t g2_dma2d_transfers,g2_dma2d_completed,g2_dma2d_errors,g2_dma2d_timeouts;
 uint32_t g2_ltdc_fifo_errors,g2_ltdc_transfer_errors;
} SV_Diagnostic;
extern volatile SV_Diagnostic g_scope_diagnostic;
void SV_Init(void);
void SV_Process(void);
uint32_t SV_IsIdle(void);
#endif
