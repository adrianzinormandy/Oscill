#include "adc_capture.h"
#include <math.h>
#include <string.h>

#define AC_QUEUE_EVENTS 8U
#define AC_OPT __attribute__((optimize("O2")))
#define AC_DMA_FLAGS (DMA_LISR_FEIF0|DMA_LISR_DMEIF0|DMA_LISR_TEIF0|DMA_LISR_HTIF0|DMA_LISR_TCIF0)
#define AC_DMA_ERRORS (DMA_LISR_FEIF0|DMA_LISR_DMEIF0|DMA_LISR_TEIF0)
/* Existing linker .bss* >RAM explicitly selects DMA-accessible SRAM; no orphan
 * section or CCM. RM0090 2.1/2.3 and 10.3.6. Verify addresses in every map. */
volatile AC_DmaStorage g_adc_dma_storage __attribute__((section(".bss.adc_capture"),aligned(32)));
uint16_t adc_snapshot[AC_BLOCK_SAMPLES] __attribute__((section(".bss.adc_snapshot"),aligned(32)));
volatile AC_Diagnostic g_adc_diagnostic;
typedef struct { uint32_t half,sequence,generation,cycles; } AC_Event;
static volatile AC_Event events[AC_QUEUE_EVENTS];
static volatile uint32_t event_head,event_tail,half_generation[2],expected_half;
static uint32_t start_tick_ms,half_period_cycles,last_ndtr,marker_high;

void AC_Snapshot(AC_Registers *r)
{
 r->adc_sr=ADC1->SR;r->adc_cr1=ADC1->CR1;r->adc_cr2=ADC1->CR2;
 r->adc_smpr2=ADC1->SMPR2;r->adc_sqr1=ADC1->SQR1;r->adc_sqr3=ADC1->SQR3;
 r->adc_ccr=ADC123_COMMON->CCR;r->tim_cr1=TIM2->CR1;r->tim_cr2=TIM2->CR2;
 r->tim_psc=TIM2->PSC;r->tim_arr=TIM2->ARR;r->tim_cnt=TIM2->CNT;r->tim_sr=TIM2->SR;
 r->dma_cr=DMA2_Stream0->CR;r->dma_ndtr=DMA2_Stream0->NDTR;
 r->dma_par=DMA2_Stream0->PAR;r->dma_m0ar=DMA2_Stream0->M0AR;
 r->dma_fcr=DMA2_Stream0->FCR;r->dma_lisr=DMA2->LISR;
 r->rcc_cfgr=RCC->CFGR;r->rcc_dckcfgr=RCC->DCKCFGR;
 r->rcc_ahb1enr=RCC->AHB1ENR;r->rcc_apb1enr=RCC->APB1ENR;r->rcc_apb2enr=RCC->APB2ENR;
}
void AC_Fail(AC_Error error)
{
 /* First inhibit triggers, preserve flags; no automatic retry/reconfiguration. */
 TIM2->CR1 &= ~TIM_CR1_CEN;
 if(g_adc_diagnostic.last_error==AC_OK) {
  g_adc_diagnostic.first_error_stage=g_adc_diagnostic.stage;
  g_adc_diagnostic.first_error_cycles=DWT->CYCCNT;
  AC_Snapshot((AC_Registers *)&g_adc_diagnostic.first_error_snapshot);
  g_adc_diagnostic.last_error=error;
 }
 g_adc_diagnostic.state=AC_FAIL;__DSB();
}
AC_Error AC_Init(void)
{
 memset((void *)&g_adc_diagnostic,0,sizeof(g_adc_diagnostic));
 g_adc_diagnostic.stage=1;g_adc_diagnostic.state=AC_RUNNING;
 g_adc_diagnostic.adc_channel=5;g_adc_diagnostic.sample_time_cycles=AC_SAMPLE_TIME_CYCLES;
 g_adc_diagnostic.dma_buffer_address=(uint32_t)adc_dma_buffer;
 g_adc_diagnostic.snapshot_address=(uint32_t)adc_snapshot;
 g_adc_diagnostic.dma_buffer_bytes=sizeof(adc_dma_buffer);
 /* RM0090 6.3.3/6.3.25: require the already validated dividers, TIMPRE=0.
  * Frequency estimate inherits operator PG8 measurement and HSI drift. */
 if(((RCC->CFGR&RCC_CFGR_PPRE1)!=(5U<<RCC_CFGR_PPRE1_Pos)) ||
    ((RCC->CFGR&RCC_CFGR_PPRE2)!=(6U<<RCC_CFGR_PPRE2_Pos)) ||
    (RCC->CFGR&RCC_CFGR_HPRE) || (RCC->DCKCFGR&RCC_DCKCFGR_TIMPRE)) {
  AC_Fail(AC_CONFIG);return AC_CONFIG;
 }
 g_adc_diagnostic.hclk_estimate_hz=AC_SDCLK_ESTIMATE_HZ*2U;
 g_adc_diagnostic.timer_clock_hz=AC_SDCLK_ESTIMATE_HZ;
 g_adc_diagnostic.adc_clock_hz=g_adc_diagnostic.hclk_estimate_hz/8U/2U;
 if((uint32_t)adc_dma_buffer<0x20000000U ||
    (uint32_t)adc_dma_buffer+sizeof(adc_dma_buffer)>0x20030000U) {
  AC_Fail(AC_CONFIG);return AC_CONFIG;
 }
 /* RM0090 6.3.10,6.3.13-14: enable only GPIOA/ADC1/TIM2/DMA2 clocks.
  * GPIOA is already enabled, preserve all unrelated bits and GPIO pins. */
 RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN|RCC_AHB1ENR_DMA2EN;
 RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
 (void)RCC->AHB1ENR;(void)RCC->APB1ENR;(void)RCC->APB2ENR;
 if((DMA2_Stream0->CR&DMA_SxCR_EN)||(TIM2->CR1&TIM_CR1_CEN)||(ADC1->CR2&ADC_CR2_ADON) ||
    (ADC2->CR2&ADC_CR2_ADON)||(ADC3->CR2&ADC_CR2_ADON)) {
  AC_Fail(AC_CONFIG);return AC_CONFIG;
 }
 /* RM0090 8.4.1/8.4.4: PA5 analog, no pulls; no other GPIO configuration. */
 GPIOA->MODER=(GPIOA->MODER&~(3U<<10))|(3U<<10);
 GPIOA->PUPDR &= ~(3U<<10);
 /* RM0090 38.16.4 / 18.3.16: freeze only TIM2 on debug halt.
  * A halt during a run invalidates wall-clock timing; tests run without halts. */
 DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM2_STOP;
 CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
 if(DWT->CTRL&DWT_CTRL_NOCYCCNT_Msk) {AC_Fail(AC_CONFIG);return AC_CONFIG;}
 DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
 /* RM0090 12.3 NVIC: new ADC/DMA IRQ priority 3; preserve existing vectors. */
 NVIC_SetPriority(DMA2_Stream0_IRQn,3U);NVIC_SetPriority(ADC_IRQn,3U);
 NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);NVIC_ClearPendingIRQ(ADC_IRQn);
 NVIC_EnableIRQ(DMA2_Stream0_IRQn);NVIC_EnableIRQ(ADC_IRQn);
 return AC_OK;
}
uint32_t AC_CheckGuards(void)
{
 for(uint32_t i=0;i<AC_GUARD_WORDS;i++) {
  if(g_adc_dma_storage.before[i]!=(0xA5C30000U+i) ||
     g_adc_dma_storage.after[i]!=(0x5A3C0000U+i)) {
   g_adc_diagnostic.guard_errors++;AC_Fail(AC_GUARD);return 0;
  }
 }
 return 1;
}
AC_Error AC_Start(uint32_t target_hz)
{
 if(g_adc_diagnostic.state==AC_FAIL || g_adc_diagnostic.capture_running ||
   (target_hz!=1000U&&target_hz!=10000U&&target_hz!=50000U&&target_hz!=100000U&&
    target_hz!=AC_FAST_RATE_HZ)) return AC_CONFIG;
 uint32_t sample_cycles=target_hz==AC_FAST_RATE_HZ?
    AC_FAST_SAMPLE_TIME_CYCLES:AC_SAMPLE_TIME_CYCLES;
 uint32_t period=(g_adc_diagnostic.timer_clock_hz+target_hz/2U)/target_hz;
 /* Conversion budget includes 2 ADCCLK trigger latency cycles: RM0090 13.5,
  * datasheet 6.3.21. Reject impossible settings rather than changing clocks. */
 if((uint64_t)period*g_adc_diagnostic.adc_clock_hz <=
    (uint64_t)(sample_cycles+12U+2U)*g_adc_diagnostic.timer_clock_hz) {
  AC_Fail(AC_CONFIG);return AC_CONFIG;
 }
 for(uint32_t i=0;i<AC_GUARD_WORDS;i++) {
  g_adc_dma_storage.before[i]=0xA5C30000U+i;g_adc_dma_storage.after[i]=0x5A3C0000U+i;
 }
 for(uint32_t i=0;i<AC_BUFFER_SAMPLES;i++) adc_dma_buffer[i]=0xFFFFU;
 event_head=event_tail=0;half_generation[0]=half_generation[1]=0;expected_half=0;
 g_adc_diagnostic.dma_ht_count=g_adc_diagnostic.dma_tc_count=0;
 g_adc_diagnostic.blocks_completed=g_adc_diagnostic.blocks_processed=g_adc_diagnostic.blocks_dropped=0;
 g_adc_diagnostic.samples_received=g_adc_diagnostic.samples_processed=0;
 g_adc_diagnostic.sample_sum=g_adc_diagnostic.sample_sum_squares=0;
 g_adc_diagnostic.queue_overflow_count=g_adc_diagnostic.stale_block_count=0;
 g_adc_diagnostic.ambiguous_irq_count=g_adc_diagnostic.sequence_error_count=0;
 g_adc_diagnostic.adc_overrun_count=g_adc_diagnostic.dma_error_count=0;
 g_adc_diagnostic.sample_min=4095;g_adc_diagnostic.sample_max=0;
 g_adc_diagnostic.max_processing_cycles=g_adc_diagnostic.max_latency_cycles=0;
 g_adc_diagnostic.ndtr_changes=g_adc_diagnostic.marker_edges=0;
 g_adc_diagnostic.irq_max_cycles=0;g_adc_diagnostic.snapshot_generation=0;
 g_adc_diagnostic.snapshot_samples=0;g_adc_diagnostic.stop_partial_samples=0;
 g_adc_diagnostic.sampling_rate_hz=g_adc_diagnostic.timer_clock_hz/period;
 g_adc_diagnostic.sample_time_cycles=sample_cycles;
 g_adc_diagnostic.psc=0;g_adc_diagnostic.arr=period-1U;
 half_period_cycles=AC_BLOCK_SAMPLES*period*2U;
 /* RM0090 18.3.1,18.4.1-2/6/11-12: MMS reset while UG loads PSC/ARR.
  * ADC EXTEN is disabled during UG; only later MMS=010 update becomes TRGO. */
 TIM2->CR1=0;TIM2->CR2=0;TIM2->SMCR=0;TIM2->DIER=0;
 ADC1->CR2=0;TIM2->PSC=0;TIM2->ARR=period-1U;TIM2->CNT=0;
 TIM2->EGR=TIM_EGR_UG;TIM2->SR=0;TIM2->CR2=TIM_CR2_MMS_1;
 /* RM0090 13.3.2-4,13.5/6/8.1,13.13.2-6/9/17: independent, /2,
  * 12-bit right aligned, one rank IN5, 56 cycles in legacy tests;
  * 3 cycles only for the fast display. No CONT/SWSTART; CCR clock unchanged.
  * ADC_CCR / SQR3: 13.13.16 / 13.13.11. */
 ADC123_COMMON->CCR=0;ADC1->CR1=ADC_CR1_OVRIE;
 ADC1->SMPR1=0;
 ADC1->SMPR2=(target_hz==AC_FAST_RATE_HZ?0U:3U)<<ADC_SMPR2_SMP5_Pos;
 ADC1->SQR1=0;ADC1->SQR2=0;ADC1->SQR3=5U;
 ADC1->SR=0;
 /* RM0090 10.3.3/8/10/17,10.5: Stream0 Channel0, direct mode, halfwords,
  * circular 2048, memory increment, high priority, HT/TC/TE/DME IRQ. */
 DMA2_Stream0->CR=DMA_SxCR_MINC|DMA_SxCR_CIRC|DMA_SxCR_PSIZE_0|DMA_SxCR_MSIZE_0|
    DMA_SxCR_PL_1|DMA_SxCR_HTIE|DMA_SxCR_TCIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE;
 DMA2_Stream0->PAR=(uint32_t)&ADC1->DR;DMA2_Stream0->M0AR=(uint32_t)adc_dma_buffer;
 DMA2_Stream0->NDTR=AC_BUFFER_SAMPLES;DMA2_Stream0->FCR=DMA_SxFCR_FEIE;
 DMA2->LIFCR=AC_DMA_FLAGS;g_adc_diagnostic.last_dma_lifcr=AC_DMA_FLAGS;
 NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);NVIC_ClearPendingIRQ(ADC_IRQn);
 marker_high=0;GPIOG->BSRR=GPIO_PIN_13<<16U;
 DMA2_Stream0->CR |= DMA_SxCR_EN;
 ADC1->CR2=ADC_CR2_DMA|ADC_CR2_DDS|ADC_CR2_EOCS|
     (6U<<ADC_CR2_EXTSEL_Pos)|ADC_CR2_EXTEN_0|ADC_CR2_ADON;
 /* ADC tSTAB max 3 us: conservative 1 HAL ms before the first timer event. */
 HAL_Delay(1U);__DSB();last_ndtr=AC_BUFFER_SAMPLES;
 start_tick_ms=HAL_GetTick();g_adc_diagnostic.capture_running=1;
 AC_Snapshot((AC_Registers *)&g_adc_diagnostic.initial);
 TIM2->CR1=TIM_CR1_ARPE|TIM_CR1_CEN;return AC_OK;
}
static void push_event(uint32_t half)
{
 uint32_t seq=++g_adc_diagnostic.blocks_completed;
 uint32_t gen=++half_generation[half];
 if(half!=expected_half) g_adc_diagnostic.sequence_error_count++;
 expected_half=half^1U;
 if(half==0) g_adc_diagnostic.dma_ht_count++;else g_adc_diagnostic.dma_tc_count++;
 /* Existing PG13 output marks every completed half; f(PG13)=Fs/2048.
  * ISR latency affects edge position; long-window mean rate is the useful check. */
 marker_high^=1U;GPIOG->BSRR=marker_high?GPIO_PIN_13:GPIO_PIN_13<<16U;
 g_adc_diagnostic.marker_edges++;
 uint32_t head=event_head;
 if(head-event_tail>=AC_QUEUE_EVENTS) {
  g_adc_diagnostic.queue_overflow_count++;g_adc_diagnostic.blocks_dropped++;
  AC_Fail(AC_QUEUE_FULL);return;
 }
 events[head%AC_QUEUE_EVENTS]=(AC_Event){half,seq,gen,DWT->CYCCNT};
 __DMB();event_head=head+1U;
}
void AC_DMA_IRQHandler(void)
{
 uint32_t begin=DWT->CYCCNT,flags=DMA2->LISR&AC_DMA_FLAGS;
 /* RM0090 10.3.13/18,10.5.1/3: save first failure before W1C.
  * No statistics, rendering or waits in either capture ISR. */
 if(flags&AC_DMA_ERRORS) {g_adc_diagnostic.dma_error_count++;AC_Fail(AC_DMA_ERROR);}
 if((flags&(DMA_LISR_HTIF0|DMA_LISR_TCIF0))==(DMA_LISR_HTIF0|DMA_LISR_TCIF0))
  g_adc_diagnostic.ambiguous_irq_count++;
 DMA2->LIFCR=flags;g_adc_diagnostic.last_dma_lifcr=flags;
 if(g_adc_diagnostic.state!=AC_FAIL) {
  if(flags&DMA_LISR_HTIF0) push_event(0);
  if(flags&DMA_LISR_TCIF0) push_event(1);
 }
 uint32_t cycles=DWT->CYCCNT-begin;
 if(cycles>g_adc_diagnostic.irq_max_cycles)g_adc_diagnostic.irq_max_cycles=cycles;
}
void AC_ADC_IRQHandler(void)
{
 if(ADC1->SR&ADC_SR_OVR) {
  g_adc_diagnostic.adc_overrun_count++;AC_Fail(AC_OVERRUN);
  ADC1->CR1 &= ~ADC_CR1_OVRIE; /* Preserve OVR, suppress repeated IRQ until stop. */
 }
}
static uint32_t half_safe(uint32_t half,uint32_t ndtr)
{return half==0 ? ndtr>0U&&ndtr<=AC_BLOCK_SAMPLES : ndtr>AC_BLOCK_SAMPLES&&ndtr<=AC_BUFFER_SAMPLES;}
AC_OPT uint32_t AC_Process(void)
{
 uint32_t ndtr=DMA2_Stream0->NDTR;
 if(ndtr!=last_ndtr) {g_adc_diagnostic.ndtr_changes++;last_ndtr=ndtr;}
 g_adc_diagnostic.last_ndtr=ndtr;
 g_adc_diagnostic.elapsed_tick_ms=HAL_GetTick()-start_tick_ms;
 if(g_adc_diagnostic.state==AC_FAIL) return 0;
 uint32_t tail=event_tail;if(tail==event_head) return 0;
 __DMB();AC_Event e=events[tail%AC_QUEUE_EVENTS];event_tail=tail+1U;
 uint32_t begin=DWT->CYCCNT,gen=half_generation[e.half];ndtr=DMA2_Stream0->NDTR;
 if(gen!=e.generation || !half_safe(e.half,ndtr)) goto stale;
 uint32_t latency=begin-e.cycles;
 if(latency>g_adc_diagnostic.max_latency_cycles)g_adc_diagnostic.max_latency_cycles=latency;
 for(uint32_t i=0;i<AC_BLOCK_SAMPLES;i++) adc_snapshot[i]=adc_dma_buffer[e.half*AC_BLOCK_SAMPLES+i];
 __DMB();ndtr=DMA2_Stream0->NDTR;
 if(half_generation[e.half]!=e.generation || !half_safe(e.half,ndtr) ||
   (DMA2->LISR&(DMA_LISR_HTIF0|DMA_LISR_TCIF0)) || DWT->CYCCNT-begin>=half_period_cycles) goto stale;
 /* Stable SRAM snapshot; no heavy work reads the DMA-owned half. */
 uint64_t sum=0,sq=0;uint32_t lo=4095,hi=0;
 for(uint32_t i=0;i<AC_BLOCK_SAMPLES;i++) {
  uint32_t v=adc_snapshot[i];if(v>4095U){AC_Fail(AC_RANGE);return 0;}
  if(v<lo)lo=v;
  if(v>hi)hi=v;
  sum+=v;sq+=(uint64_t)v*v;
 }
 g_adc_diagnostic.sample_sum+=sum;g_adc_diagnostic.sample_sum_squares+=sq;
 g_adc_diagnostic.samples_processed+=AC_BLOCK_SAMPLES;
 if(lo<g_adc_diagnostic.sample_min)g_adc_diagnostic.sample_min=lo;
 if(hi>g_adc_diagnostic.sample_max)g_adc_diagnostic.sample_max=hi;
 g_adc_diagnostic.sample_peak_to_peak=g_adc_diagnostic.sample_max-g_adc_diagnostic.sample_min;
 g_adc_diagnostic.blocks_processed++;g_adc_diagnostic.snapshot_generation++;
 g_adc_diagnostic.snapshot_samples=AC_BLOCK_SAMPLES;
 g_adc_diagnostic.latest_block_sequence=e.sequence;g_adc_diagnostic.latest_block_cycles=e.cycles;
 g_adc_diagnostic.samples_received=(uint64_t)g_adc_diagnostic.blocks_completed*AC_BLOCK_SAMPLES;
 uint32_t cycles=DWT->CYCCNT-begin;
 if(cycles>g_adc_diagnostic.max_processing_cycles)g_adc_diagnostic.max_processing_cycles=cycles;
 return 1;
stale:
 g_adc_diagnostic.stale_block_count++;g_adc_diagnostic.blocks_dropped++;
 g_adc_diagnostic.snapshot_samples=0;AC_Fail(AC_BLOCK_STALE);return 0;
}
AC_Error AC_Stop(void)
{
 /* RM0090 13.3.1,10.3.14: inhibit new TRGO, allow last conversion/DMA to
  * settle before disabling ADC and Stream0; retain any pending complete block. */
 TIM2->CR1 &= ~TIM_CR1_CEN;__DSB();HAL_Delay(1U);
 ADC1->CR2 &= ~(ADC_CR2_EXTEN|ADC_CR2_ADON|ADC_CR2_DMA|ADC_CR2_DDS);
 /* RM0090 10.3.14: clearing EN itself sets TCIF, even for a partial block.
  * Mask stream IRQ sources before software suspension, so this synthetic TC
  * cannot be published as a completed 1024-sample block. ADC is quiescent. */
 DMA2_Stream0->CR &= ~(DMA_SxCR_HTIE|DMA_SxCR_TCIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE);
 DMA2_Stream0->FCR &= ~DMA_SxFCR_FEIE;__DSB();
 DMA2_Stream0->CR &= ~DMA_SxCR_EN;
 uint32_t begin=DWT->CYCCNT;
 while(DMA2_Stream0->CR&DMA_SxCR_EN) {
  if(DWT->CYCCNT-begin>SystemCoreClock/1000U) {AC_Fail(AC_STOP_TIMEOUT);return AC_STOP_TIMEOUT;}
 }
 g_adc_diagnostic.capture_running=0;
 g_adc_diagnostic.elapsed_tick_ms=HAL_GetTick()-start_tick_ms;
 uint32_t n=DMA2_Stream0->NDTR;
 g_adc_diagnostic.stop_partial_samples=(AC_BUFFER_SAMPLES-n)%AC_BLOCK_SAMPLES;
 g_adc_diagnostic.samples_received=(uint64_t)g_adc_diagnostic.blocks_completed*AC_BLOCK_SAMPLES+
    g_adc_diagnostic.stop_partial_samples;
 AC_Snapshot((AC_Registers *)&g_adc_diagnostic.current);
 DMA2->LIFCR=AC_DMA_FLAGS;g_adc_diagnostic.last_dma_lifcr=AC_DMA_FLAGS;
 NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
 return AC_CheckGuards()?AC_OK:AC_GUARD;
}
void AC_SaveResult(AC_TestResult *r)
{
 uint64_t n=g_adc_diagnostic.samples_processed;
 double mean=n?(double)g_adc_diagnostic.sample_sum/(double)n:0;
 /* Population standard deviation = RMS of AC component; not total RMS. */
 double variance=n?(double)g_adc_diagnostic.sample_sum_squares/(double)n-mean*mean:0;
 g_adc_diagnostic.sample_mean=(float)mean;
 g_adc_diagnostic.sample_rms=(float)sqrt(variance>0?variance:0);
 *r=(AC_TestResult){.rate_target_hz=0,.rate_estimate_hz=g_adc_diagnostic.sampling_rate_hz,
  .psc=g_adc_diagnostic.psc,.arr=g_adc_diagnostic.arr,.elapsed_tick_ms=g_adc_diagnostic.elapsed_tick_ms,
  .ht=g_adc_diagnostic.dma_ht_count,.tc=g_adc_diagnostic.dma_tc_count,
  .blocks=g_adc_diagnostic.blocks_completed,.processed=g_adc_diagnostic.blocks_processed,
  .dropped=g_adc_diagnostic.blocks_dropped,.queue_overflow=g_adc_diagnostic.queue_overflow_count,
  .stale=g_adc_diagnostic.stale_block_count,.ambiguous=g_adc_diagnostic.ambiguous_irq_count,
  .samples_received=g_adc_diagnostic.samples_received,.samples_processed=n,
  .ndtr_changes=g_adc_diagnostic.ndtr_changes,.guard_ok=AC_CheckGuards(),
  .overrun=g_adc_diagnostic.adc_overrun_count,.dma_errors=g_adc_diagnostic.dma_error_count,
  .sequence_errors=g_adc_diagnostic.sequence_error_count,
  .min=g_adc_diagnostic.sample_min,.max=g_adc_diagnostic.sample_max,
  .peak_to_peak=g_adc_diagnostic.sample_peak_to_peak,
  .max_processing_cycles=g_adc_diagnostic.max_processing_cycles,
  .max_latency_cycles=g_adc_diagnostic.max_latency_cycles,
  .mean=(float)mean,.rms_ac=g_adc_diagnostic.sample_rms,
  .sum=g_adc_diagnostic.sample_sum,.sum_squares=g_adc_diagnostic.sample_sum_squares,
  .before=g_adc_diagnostic.initial,.after=g_adc_diagnostic.current};
}
void AC_RecordHardFault(void)
{
 g_adc_diagnostic.hardfault_count++;g_adc_diagnostic.cfsr=SCB->CFSR;
 g_adc_diagnostic.hfsr=SCB->HFSR;g_adc_diagnostic.bfar=SCB->BFAR;AC_Fail(AC_HARDFAULT);
}
__attribute__((noinline)) void AC_Checkpoint(void){__NOP();}
__attribute__((noinline,noreturn)) void AC_DiagnosticStop(void){for(;;)__WFI();}
