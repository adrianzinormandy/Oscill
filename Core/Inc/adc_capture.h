#ifndef OSCILL_ADC_CAPTURE_H
#define OSCILL_ADC_CAPTURE_H
#include "main.h"
#include "sdram.h"
#include "lcd.h"

#ifndef ADC_CAPTURE_DIAGNOSTIC_ENABLE
#define ADC_CAPTURE_DIAGNOSTIC_ENABLE 1U
#endif
#define AC_BUFFER_SAMPLES 2048U
#define AC_BLOCK_SAMPLES 1024U
#define AC_GUARD_WORDS 8U
/* Operator PG8 measurement from task 7; estimate, not a new measurement. */
#define AC_SDCLK_ESTIMATE_HZ 81300000U
#define AC_SAMPLE_TIME_CYCLES 56U
/* Fast display: preserve RCC, shorten IN5 acquisition only for this rate.
 * RM0090 13.5/13.13.5: 3 + 12 conversion cycles, low-impedance source required. */
#define AC_FAST_RATE_HZ 500000U
#define AC_FAST_SAMPLE_TIME_CYCLES 3U
#define AC_DISPLAY_RATE_HZ AC_FAST_RATE_HZ
#define AC_TEST_RATE_COUNT 4U
/* 1: digital + static load; 2: physical checkpoint; 3: live LCD + 63 s G2.
 * Physical input is prepared and measured before reset/flash. */
#ifndef ADC_CAPTURE_TEST_PHASE
#define ADC_CAPTURE_TEST_PHASE 3U
#endif
typedef enum { AC_NOT_STARTED=0, AC_RUNNING=1, AC_DIGITAL_PASS=2, AC_FAIL=3 } AC_State;
typedef enum { AC_OK=0, AC_CONFIG=1, AC_DMA_ERROR=2, AC_OVERRUN=3,
 AC_QUEUE_FULL=4, AC_BLOCK_STALE=5, AC_SEQUENCE=6, AC_GUARD=7,
 AC_STOP_TIMEOUT=8, AC_NO_PROGRESS=9, AC_RANGE=10, AC_GRAPHICS=11,
 AC_HARDFAULT=12 } AC_Error;
typedef struct {
 uint32_t adc_sr,adc_cr1,adc_cr2,adc_smpr2,adc_sqr1,adc_sqr3,adc_ccr;
 uint32_t tim_cr1,tim_cr2,tim_psc,tim_arr,tim_cnt,tim_sr;
 uint32_t dma_cr,dma_ndtr,dma_par,dma_m0ar,dma_fcr,dma_lisr;
 uint32_t rcc_cfgr,rcc_dckcfgr,rcc_ahb1enr,rcc_apb1enr,rcc_apb2enr;
} AC_Registers;
typedef struct {
 uint32_t rate_target_hz,rate_estimate_hz,psc,arr,elapsed_tick_ms;
 uint32_t ht,tc,blocks,processed,dropped,queue_overflow,stale,ambiguous;
 uint64_t samples_received,samples_processed;
 uint32_t ndtr_changes,guard_ok,overrun,dma_errors,sequence_errors;
 uint32_t min,max,peak_to_peak,max_processing_cycles,max_latency_cycles;
 float mean,rms_ac,frequency_hz;
 uint64_t sum,sum_squares;
 AC_Registers before,after;
} AC_TestResult;
typedef struct {
 uint32_t stage,state,last_error,adc_channel,adc_clock_hz,timer_clock_hz;
 uint32_t sampling_rate_hz,sample_time_cycles,psc,arr,hclk_estimate_hz;
 uint32_t dma_buffer_address,snapshot_address,dma_buffer_bytes;
 uint32_t dma_ht_count,dma_tc_count,blocks_completed,blocks_processed,blocks_dropped;
 uint32_t queue_overflow_count,stale_block_count,ambiguous_irq_count,sequence_error_count;
 uint32_t adc_overrun_count,dma_error_count,guard_errors,ndtr_changes;
 uint32_t sample_min,sample_max,sample_peak_to_peak,max_processing_cycles,max_latency_cycles;
 uint64_t samples_received,samples_processed,sample_sum,sample_sum_squares;
 float sample_mean,sample_rms,signal_frequency_hz;
 uint32_t snapshot_generation,display_frames,hardfault_count,cfsr,hfsr,bfar;
 uint32_t capture_running,elapsed_tick_ms,last_ndtr,last_dma_lifcr,marker_edges;
 uint32_t latest_block_sequence,latest_block_cycles,irq_max_cycles;
 uint32_t first_error_stage,first_error_cycles,tests_passed,physical_state;
 uint32_t snapshot_samples,stop_partial_samples;
 AC_Registers initial,current,first_error_snapshot;
 SDRAM_RegisterSnapshot fmc_before,fmc_after;
 LCD_RegisterSnapshot ltdc_before,ltdc_after;
 uint32_t pllsai_before,pllsai_after,dckcfgr_before,dckcfgr_after;
 AC_TestResult rates[AC_TEST_RATE_COUNT],static_load,graphics_load;
} AC_Diagnostic;
typedef struct {
 uint32_t before[AC_GUARD_WORDS];
 uint16_t samples[AC_BUFFER_SAMPLES];
 uint32_t after[AC_GUARD_WORDS];
} AC_DmaStorage;
extern volatile AC_DmaStorage g_adc_dma_storage;
#define adc_dma_buffer (g_adc_dma_storage.samples)
extern uint16_t adc_snapshot[AC_BLOCK_SAMPLES];
extern volatile AC_Diagnostic g_adc_diagnostic;
AC_Error AC_Init(void);
AC_Error AC_Start(uint32_t target_hz);
AC_Error AC_Stop(void);
uint32_t AC_Process(void);
void AC_DMA_IRQHandler(void);
void AC_ADC_IRQHandler(void);
void AC_Snapshot(AC_Registers *r);
void AC_SaveResult(AC_TestResult *r);
uint32_t AC_CheckGuards(void);
void AC_Fail(AC_Error error);
void AC_RecordHardFault(void);
void AC_TestRun(void);
void AC_TestProcess(void);
__attribute__((noinline)) void AC_Checkpoint(void);
__attribute__((noinline,noreturn)) void AC_DiagnosticStop(void);
#endif
