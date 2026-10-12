#include "adc_capture.h"
#include "dma2d_graphics.h"
#include <string.h>

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
void AC_TestRun(void)
{
 if(AC_Init()!=AC_OK){AC_DiagnosticStop();}
 /* Keep task-7 LTDC/framebuffer static; initialize the preserved DG API only,
  * do not run old benchmarks or any additional DMA2D transfers in this step. */
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
#else
 /* Reuse unchanged capture settings for the physical DC/signal checkpoint.
  * ADC signal accuracy is assessed against operator measurements in the report,
  * not made PASS by an arbitrary threshold here. */
 g_adc_diagnostic.stage=8;
 run_rate(10000U,7000U,(AC_TestResult *)&g_adc_diagnostic.rates[0]);
#endif
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
 g_adc_diagnostic.stage=7;g_adc_diagnostic.state=AC_DIGITAL_PASS;
 AC_Checkpoint();
}
void AC_TestProcess(void)
{
 /* Stopped digital checkpoint. Physical DC/signal tests are a separate step. */
 __WFI();
}
