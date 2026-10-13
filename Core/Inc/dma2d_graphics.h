#ifndef OSCILL_DMA2D_GRAPHICS_H
#define OSCILL_DMA2D_GRAPHICS_H
#include "lcd.h"

#define DMA2D_DIAGNOSTIC_ENABLE 1U
#define DMA2D_BENCH_SAMPLES 30U
#define DMA2D_BENCH_COUNT 8U
typedef enum { DG_OK=0, DG_INVALID=1, DG_BUSY=2, DG_HAL_ERROR=3,
  DG_TIMEOUT=4, DG_VERIFY=5, DG_CONFIG_CHANGED=6, DG_HARDFAULT=7 } DG_Status;
typedef enum { DG_FILL=0, DG_COPY=1, DG_PFC=2, DG_BLEND=3 } DG_Operation;
typedef struct { uint32_t address, width, height, format; } DG_Surface;
typedef struct {
  DG_Operation operation;
  const DG_Surface *dst, *fg, *bg;
  uint32_t dx, dy, fx, fy, bx, by, width, height;
  uint16_t color;
} DG_Job;
typedef struct {
  uint32_t cr,isr,opfccr,ocolr,omar,oor,nlr,fgmar,fgor,fgpfccr,bgmar,bgor,bgpfccr;
} DG_Registers;
typedef struct {
  uint32_t cpu, setup, launch, tail, total;
} DG_Sample;
typedef struct {
  uint32_t operation,width,height,samples;
  DG_Sample raw[DMA2D_BENCH_SAMPLES];
} DG_Benchmark;
typedef struct {
  uint32_t stage,state,current_mode,transfer_count,completed_count,irq_count;
  uint32_t dma2d_error_count,timeout_count,verification_errors,first_error_addr;
  uint32_t expected,actual,last_hal_status,last_hal_error,last_dma2d_isr,last_error;
  uint64_t pixels_processed;
  uint32_t cpu_cycles,dma2d_cycles,ltdc_fifo_errors,ltdc_transfer_errors,fmc_unchanged;
  uint32_t hardfault_count,cfsr,hfsr,bfar_valid,bfar;
  uint32_t source_addr,background_addr,destination_addr,busy_rejections,invalid_rejections;
  uint32_t guard_checks,checked_pixels,tests_passed,stage_pass_mask;
  uint32_t setup_cycles,launch_cycles,tail_cycles,total_cycles;
  uint32_t dwt_ctrl,dwt_probe_cycles,nominal_hclk_hz;
  uint32_t async_inflight,async_returned_pending,overlap_batches,overlap_cpu_accesses;
  uint32_t stress_start_tick_ms,stress_elapsed_tick_ms,stress_transfers;
  uint64_t stress_pixels;
  uint32_t framebuffer_checks,animation_updates,display_phase,visual_state;
  uint32_t first_error_stage,first_error_mode,first_error_isr;
  uint32_t blend_rounding_pixels,blend_max_channel_difference;
  DG_Registers before,after,first_error_registers;
  SDRAM_RegisterSnapshot fmc_before,fmc_after;
  LCD_RegisterSnapshot ltdc_before,ltdc_after;
  DG_Benchmark benchmarks[DMA2D_BENCH_COUNT];
} DG_Diagnostic;
extern volatile DG_Diagnostic g_dma2d_diagnostic;
extern const DG_Surface dg_framebuffer,dg_backbuffer,dg_scratch_a,dg_scratch_b,dg_argb_source,dg_argb_destination;
DG_Status DG_Init(void);
DG_Status DG_Start(const DG_Job *job, uint32_t interrupt_mode);
DG_Status DG_Wait(uint32_t timeout_ms);
uint32_t DG_IsBusy(void);
void DG_IRQHandler(void);
void DG_RecordHardFault(void);
void DG_Fail(DG_Status status);
void DG_Snapshot(DG_Registers *s);
uint32_t DG_Expand565(uint16_t value);
uint16_t DG_Quantize565(uint32_t value);
uint16_t DG_Blend565(uint32_t foreground,uint16_t background);
void DG_TestRun(void);
void DG_TestProcess(void);
__attribute__((noinline,noreturn)) void DG_DiagnosticStop(void);
/* Hardware checkpoint: no side effects; breakpoint here after each stage. */
__attribute__((noinline)) void DG_Checkpoint(void);
#endif
