#ifndef OSCILL_SCOPE_VIEW_H
#define OSCILL_SCOPE_VIEW_H
#include "adc_capture.h"

#define SV_FRAME_PERIOD_TICK_MS 67U
#define SV_LOAD_DURATION_TICK_MS 63000U
#define SV_TIME_DIV_US 20U /* Target label; actual scale = 10 / measured Fs seconds/div. */
#define SV_WINDOW_INTERVALS_SAMPLES 100U
#define SV_WINDOW_SAMPLES (SV_WINDOW_INTERVALS_SAMPLES+1U)
#define SV_PRETRIGGER_SAMPLES 20U
#define SV_TRIGGER_LEVEL_CODE 2048U
#define SV_TRIGGER_HYSTERESIS_CODE 32U
#define SV_BUTTON_DEBOUNCE_TICK_MS 30U
#define SV_TRIGGER_WAIT_TICK_MS 250U
typedef enum { SV_TRIGGER_OFF=0, SV_TRIGGER_RISING=1 } SV_TriggerMode;
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
 /* Append to preserve the offsets of historical G2 fields. */
 uint32_t logical_width_pixels,logical_height_pixels,time_div_target_us;
 uint32_t window_intervals_samples,time_div_estimate_ns;
 uint32_t trigger_mode,trigger_level_code,trigger_hysteresis_code;
 uint32_t trigger_matches,trigger_misses,trigger_waiting,trigger_q16;
 uint32_t button_presses,algorithm_checks,algorithm_failures;
 uint32_t rendered_trigger_mode,rendered_trigger_waiting,triggered_frames;
 uint32_t render_pending,render_slice_max_cycles;
 uint32_t frontbuffer_address,backbuffer_address,swap_requests,swap_completed;
 uint32_t reload_start_tick_ms,reload_timeouts,reload_max_tick_ms;
} SV_Diagnostic;
extern volatile SV_Diagnostic g_scope_diagnostic;
void SV_Init(void);
void SV_Process(void);
uint32_t SV_IsIdle(void);
uint32_t SV_NeedsService(void);
#endif
