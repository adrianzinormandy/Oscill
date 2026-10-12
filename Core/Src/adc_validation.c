#include "adc_capture.h"
#include "dma2d_graphics.h"
#include "scope_view.h"
#include <string.h>

/* Single physical capture rate. Override only for the documented 50/100 kSPS
 * repeats (RM0090 18.4.11-12: PSC/ARR); ADC sampling time and RCC stay unchanged. */
#ifndef ADC_CAPTURE_PHYSICAL_RATE_HZ
#define ADC_CAPTURE_PHYSICAL_RATE_HZ 10000U
#endif

static void fmc_snapshot(volatile SDRAM_RegisterSnapshot *s)
{
 s->sdcr1=FMC_Bank5_6->SDCR[0];s->sdcr2=FMC_Bank5_6->SDCR[1];
 s->sdtr1=FMC_Bank5_6->SDTR[0];s->sdtr2=FMC_Bank5_6->SDTR[1];
 s->sdrtr=FMC_Bank5_6->SDRTR;s->sdsr=FMC_Bank5_6->SDSR;
}
static void ltdc_snapshot(volatile LCD_RegisterSnapshot *s)
{
 s->sscr=LTDC->SSCR;s->bpcr=LTDC->BPCR;s->awcr=LTDC->AWCR;s->twcr=LTDC->TWCR;
 s->gcr=LTDC->GCR;s->srcr=LTDC->SRCR;s->ier=LTDC->IER;s->isr=LTDC->ISR;
 s->cpsr=LTDC->CPSR;s->cdsr=LTDC->CDSR;
 s->l1cr=LTDC_Layer1->CR;s->l1whpcr=LTDC_Layer1->WHPCR;s->l1wvpcr=LTDC_Layer1->WVPCR;
 s->l1pfcr=LTDC_Layer1->PFCR;s->l1cacr=LTDC_Layer1->CACR;s->l1bfcr=LTDC_Layer1->BFCR;
 s->l1cfbar=LTDC_Layer1->CFBAR;s->l1cfblr=LTDC_Layer1->CFBLR;
 s->l1cfblnr=LTDC_Layer1->CFBLNR;s->l2cr=LTDC_Layer2->CR;
}

#if ADC_CAPTURE_TEST_PHASE != 3U
static void run_rate(uint32_t hz,uint32_t duration_tick_ms,AC_TestResult *result)
{
 if(AC_Start(hz)!=AC_OK){AC_DiagnosticStop();}
 uint32_t begin=HAL_GetTick(),progress=begin,blocks=0;
 while(HAL_GetTick()-begin<duration_tick_ms) {
  AC_Process();
  if(g_adc_diagnostic.state==AC_FAIL || g_lcd_diagnostic.state==LCD_STATE_FAIL)
   {AC_DiagnosticStop();}
  if(g_adc_diagnostic.blocks_completed!=blocks) {
   blocks=g_adc_diagnostic.blocks_completed;progress=HAL_GetTick();
  }
  if(HAL_GetTick()-progress>2200U){AC_Fail(AC_NO_PROGRESS);AC_DiagnosticStop();}
 }
 /* Choose a completed-block boundary; stop only after its snapshot is handled. */
 uint32_t next=g_adc_diagnostic.blocks_completed;
 while(g_adc_diagnostic.blocks_completed==next) {
  AC_Process();if(g_adc_diagnostic.state==AC_FAIL){AC_DiagnosticStop();}
 }
 while(AC_Process()){}
 if(AC_Stop()!=AC_OK||g_adc_diagnostic.state==AC_FAIL){AC_DiagnosticStop();}
 AC_SaveResult(result);result->rate_target_hz=hz;
 if(result->blocks<48U||result->processed!=result->blocks||result->dropped||
    result->ambiguous||result->sequence_errors||!result->ndtr_changes||!result->guard_ok||
    result->overrun||result->dma_errors) {
  AC_Fail(AC_SEQUENCE);AC_DiagnosticStop();
 }
 g_adc_diagnostic.tests_passed++;AC_Checkpoint();
}
#endif

static void check_existing_config(void)
{
 fmc_snapshot(&g_adc_diagnostic.fmc_after);
 ltdc_snapshot(&g_adc_diagnostic.ltdc_after);
 g_adc_diagnostic.pllsai_after=RCC->PLLSAICFGR;
 g_adc_diagnostic.dckcfgr_after=RCC->DCKCFGR;
 if(memcmp((void *)&g_adc_diagnostic.fmc_before,(void *)&g_adc_diagnostic.fmc_after,
    sizeof(SDRAM_RegisterSnapshot)-sizeof(uint32_t)) ||
    memcmp((void *)&g_adc_diagnostic.ltdc_before,(void *)&g_adc_diagnostic.ltdc_after,
      5U*sizeof(uint32_t)) ||
    memcmp((void *)&g_adc_diagnostic.ltdc_before.l1cr,(void *)&g_adc_diagnostic.ltdc_after.l1cr,
      10U*sizeof(uint32_t)) ||
    g_adc_diagnostic.ltdc_before.ier!=g_adc_diagnostic.ltdc_after.ier ||
    g_adc_diagnostic.pllsai_before!=g_adc_diagnostic.pllsai_after ||
    g_adc_diagnostic.dckcfgr_before!=g_adc_diagnostic.dckcfgr_after ||
    g_lcd_diagnostic.fifo_underrun_count||g_lcd_diagnostic.transfer_error_count) {
  AC_Fail(AC_CONFIG);AC_DiagnosticStop();
 }
}
void AC_TestRun(void)
{
 if(AC_Init()!=AC_OK){AC_DiagnosticStop();}
 /* Reuse the task-7 DG API; no old benchmarks or clock/GPIO changes. */
 if(DG_Init()!=DG_OK){AC_Fail(AC_GRAPHICS);AC_DiagnosticStop();}
 fmc_snapshot(&g_adc_diagnostic.fmc_before);
 ltdc_snapshot(&g_adc_diagnostic.ltdc_before);
 g_adc_diagnostic.pllsai_before=RCC->PLLSAICFGR;
 g_adc_diagnostic.dckcfgr_before=RCC->DCKCFGR;
#if ADC_CAPTURE_TEST_PHASE == 1U
 static const uint32_t rates[4]={1000,10000,50000,100000};
 static const uint32_t durations[4]={51000,5200,1200,700};
 for(uint32_t i=0;i<4;i++) {
  g_adc_diagnostic.stage=2U+i;
  run_rate(rates[i],durations[i],(AC_TestResult *)&g_adc_diagnostic.rates[i]);
 }
 g_adc_diagnostic.stage=6;
 run_rate(100000U,63000U,(AC_TestResult *)&g_adc_diagnostic.static_load);
#elif ADC_CAPTURE_TEST_PHASE == 2U
 /* Reuse unchanged capture settings for the physical DC/signal checkpoint.
  * ADC signal accuracy is assessed against operator measurements in the report,
  * not made PASS by an arbitrary threshold here. */
 g_adc_diagnostic.stage=8;
 run_rate(ADC_CAPTURE_PHYSICAL_RATE_HZ,7000U,(AC_TestResult *)&g_adc_diagnostic.rates[0]);
#elif ADC_CAPTURE_TEST_PHASE == 3U
 g_adc_diagnostic.stage=9U;
 SV_Init();
 if(AC_Start(100000U)!=AC_OK){AC_Fail(AC_CONFIG);AC_DiagnosticStop();}
 g_scope_diagnostic.start_tick_ms=HAL_GetTick();
 g_scope_diagnostic.progress_tick_ms=HAL_GetTick();
 return;
#else
#error Unsupported ADC_CAPTURE_TEST_PHASE
#endif
 check_existing_config();
 g_adc_diagnostic.stage=7;g_adc_diagnostic.state=AC_DIGITAL_PASS;
 AC_Checkpoint();
}
void AC_TestProcess(void)
{
#if ADC_CAPTURE_TEST_PHASE == 3U
 uint32_t begin=DWT->CYCCNT;
 uint32_t processed=AC_Process(),cycles=DWT->CYCCNT-begin;
 g_scope_diagnostic.capture_work_cycles+=cycles;
 if(cycles>g_scope_diagnostic.capture_call_max_cycles)g_scope_diagnostic.capture_call_max_cycles=cycles;
 begin=DWT->CYCCNT;SV_Process();cycles=DWT->CYCCNT-begin;
 g_scope_diagnostic.graphics_work_cycles+=cycles;
 if(cycles>g_scope_diagnostic.graphics_call_max_cycles)g_scope_diagnostic.graphics_call_max_cycles=cycles;
 uint32_t now=HAL_GetTick();
 if(g_adc_diagnostic.state==AC_FAIL||g_dma2d_diagnostic.state==LCD_STATE_FAIL||
    g_lcd_diagnostic.state==LCD_STATE_FAIL){AC_Fail(AC_GRAPHICS);AC_DiagnosticStop();}
 if(g_scope_diagnostic.progress_blocks!=g_adc_diagnostic.blocks_completed) {
  g_scope_diagnostic.progress_blocks=g_adc_diagnostic.blocks_completed;
  g_scope_diagnostic.progress_tick_ms=now;
 }
 if(now-g_scope_diagnostic.progress_tick_ms>2200U){AC_Fail(AC_NO_PROGRESS);AC_DiagnosticStop();}
 g_scope_diagnostic.elapsed_tick_ms=now-g_scope_diagnostic.start_tick_ms;
 if(!g_scope_diagnostic.g2_complete&&g_scope_diagnostic.elapsed_tick_ms>=SV_LOAD_DURATION_TICK_MS&&
    processed&&SV_IsIdle()) {
  /* End at a processed block boundary. Save G2 separately, then restart the
   * same capture settings for live display; never retry an error automatically. */
  if(AC_Stop()!=AC_OK){AC_DiagnosticStop();}
  AC_TestResult *r=(AC_TestResult *)&g_adc_diagnostic.graphics_load;
  AC_SaveResult(r);r->rate_target_hz=100000U;
  if(r->processed!=r->blocks||r->dropped||r->queue_overflow||r->stale||r->ambiguous||
     r->sequence_errors||!r->guard_ok||r->overrun||r->dma_errors) {
   AC_Fail(AC_SEQUENCE);AC_DiagnosticStop();
  }
  check_existing_config();
  g_scope_diagnostic.g2_frames=g_scope_diagnostic.frames;
  g_scope_diagnostic.g2_elapsed_tick_ms=r->elapsed_tick_ms;
  g_scope_diagnostic.g2_capture_work_cycles=g_scope_diagnostic.capture_work_cycles;
  g_scope_diagnostic.g2_graphics_work_cycles=g_scope_diagnostic.graphics_work_cycles;
  g_scope_diagnostic.g2_render_max_cycles=g_scope_diagnostic.render_max_cycles;
  g_scope_diagnostic.g2_capture_call_max_cycles=g_scope_diagnostic.capture_call_max_cycles;
  g_scope_diagnostic.g2_graphics_call_max_cycles=g_scope_diagnostic.graphics_call_max_cycles;
  g_scope_diagnostic.g2_dma2d_transfers=g_dma2d_diagnostic.transfer_count;
  g_scope_diagnostic.g2_dma2d_completed=g_dma2d_diagnostic.completed_count;
  g_scope_diagnostic.g2_dma2d_errors=g_dma2d_diagnostic.dma2d_error_count;
  g_scope_diagnostic.g2_dma2d_timeouts=g_dma2d_diagnostic.timeout_count;
  g_scope_diagnostic.g2_ltdc_fifo_errors=g_lcd_diagnostic.fifo_underrun_count;
  g_scope_diagnostic.g2_ltdc_transfer_errors=g_lcd_diagnostic.transfer_error_count;
  if(!g_scope_diagnostic.g2_frames||g_scope_diagnostic.g2_dma2d_transfers!=
     g_scope_diagnostic.g2_dma2d_completed){AC_Fail(AC_GRAPHICS);AC_DiagnosticStop();}
  g_adc_diagnostic.tests_passed++;g_adc_diagnostic.stage=10U;
  if(AC_Start(100000U)!=AC_OK){AC_Fail(AC_CONFIG);AC_DiagnosticStop();}
  g_scope_diagnostic.progress_blocks=0U;g_scope_diagnostic.progress_tick_ms=HAL_GetTick();
  g_scope_diagnostic.frame_generation=0U;
  __DMB();g_scope_diagnostic.g2_complete=1U;AC_Checkpoint();
 }
#endif
 __WFI();
}
