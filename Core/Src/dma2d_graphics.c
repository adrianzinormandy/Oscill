#include "dma2d_graphics.h"
#include "main.h"
extern DMA2D_HandleTypeDef hdma2d;
volatile DG_Diagnostic g_dma2d_diagnostic __attribute__((section(".bss.dma2d_diagnostic"),aligned(8)));
const DG_Surface dg_framebuffer={0xD0000000U,240U,320U,DMA2D_OUTPUT_RGB565};
const DG_Surface dg_scratch_a={0xD0200000U,240U,320U,DMA2D_OUTPUT_RGB565};
const DG_Surface dg_scratch_b={0xD0240000U,240U,320U,DMA2D_OUTPUT_RGB565};
const DG_Surface dg_argb_source={0xD0300000U,64U,64U,DMA2D_OUTPUT_ARGB8888};
const DG_Surface dg_argb_destination={0xD0310000U,64U,64U,DMA2D_OUTPUT_ARGB8888};
static uint32_t start_tick_ms,start_cycles,call_cycles;
static uint32_t pending_pixels;
static volatile uint32_t irq_done;
static volatile uint32_t irq_cycles;
static uint32_t transaction_open,active_irq_mode;
static uint32_t bytes_per_pixel(const DG_Surface *s) {return s->format==DMA2D_OUTPUT_RGB565?2U:4U;}

/* RM0090 11.3.4: replicate high bits, implicit alpha=255 for RGB565. */
__attribute__((optimize("O2"))) uint32_t DG_Expand565(uint16_t v)
{
  uint32_t r=(v>>11)&31U,g=(v>>5)&63U,b=v&31U;
  return 0xFF000000U|(((r<<3)|(r>>2))<<16)|(((g<<2)|(g>>4))<<8)|((b<<3)|(b>>2));
}
/* RM0090 11.3.7: retain most significant output component bits. */
__attribute__((optimize("O2"))) uint16_t DG_Quantize565(uint32_t v)
{return (uint16_t)(((v>>8)&0xF800U)|((v>>5)&0x07E0U)|((v>>3)&0x001FU));}
/* RM0090 11.3.6: opaque BG => floor((FG*a+BG*(255-a))/255). */
__attribute__((optimize("O2"))) uint16_t DG_Blend565(uint32_t f,uint16_t b)
{
  uint32_t bg=DG_Expand565(b),a=f>>24,r=0U;
  for(uint32_t shift=0U;shift<24U;shift+=8U) {
    r|=(( ((f>>shift)&255U)*a+((bg>>shift)&255U)*(255U-a))/255U)<<shift;
  }
  return DG_Quantize565(r);
}
void DG_Snapshot(DG_Registers *s)
{
  /* RM0090 11.5: capture before any HAL handler can clear error flags. */
  s->cr=DMA2D->CR;s->isr=DMA2D->ISR;s->opfccr=DMA2D->OPFCCR;s->ocolr=DMA2D->OCOLR;
  s->omar=DMA2D->OMAR;s->oor=DMA2D->OOR;s->nlr=DMA2D->NLR;
  s->fgmar=DMA2D->FGMAR;s->fgor=DMA2D->FGOR;s->fgpfccr=DMA2D->FGPFCCR;
  s->bgmar=DMA2D->BGMAR;s->bgor=DMA2D->BGOR;s->bgpfccr=DMA2D->BGPFCCR;
}
void DG_Fail(DG_Status status)
{
  if(g_dma2d_diagnostic.state!=LCD_STATE_FAIL) {
    g_dma2d_diagnostic.first_error_stage=g_dma2d_diagnostic.stage;
    g_dma2d_diagnostic.first_error_mode=g_dma2d_diagnostic.current_mode;
    if((RCC->AHB1ENR&RCC_AHB1ENR_DMA2DEN)!=0U) {
      DG_Registers r;DG_Snapshot(&r);g_dma2d_diagnostic.first_error_registers=r;
      g_dma2d_diagnostic.first_error_isr=r.isr;
    }
  }
  g_dma2d_diagnostic.last_error=status;g_dma2d_diagnostic.state=LCD_STATE_FAIL;
}
static uint32_t surface_valid(const DG_Surface *s)
{
  /* Whitelist fixed DMA-accessible buffers. No CCM or arbitrary peripheral address. */
  if(s==NULL) return 0U;
  const DG_Surface *all[]={&dg_framebuffer,&dg_scratch_a,&dg_scratch_b,&dg_argb_source,&dg_argb_destination};
  for(uint32_t i=0U;i<5U;i++) if(s->address==all[i]->address&&s->width==all[i]->width
      &&s->height==all[i]->height&&s->format==all[i]->format) return 1U;
  return 0U;
}
static uint32_t rectangle_valid(const DG_Surface *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{return surface_valid(s)&&w!=0U&&h!=0U&&x<s->width&&y<s->height&&w<=s->width-x&&h<=s->height-y;}
static uint32_t address(const DG_Surface *s,uint32_t x,uint32_t y)
{return s->address+(y*s->width+x)*bytes_per_pixel(s);}
static uint32_t overlaps(const DG_Surface *a,uint32_t ax,uint32_t ay,
                         const DG_Surface *b,uint32_t bx,uint32_t by,uint32_t w,uint32_t h)
{
  uint64_t a0=address(a,ax,ay),b0=address(b,bx,by);
  uint64_t a1=a0+((uint64_t)(h-1U)*a->width+w)*bytes_per_pixel(a);
  uint64_t b1=b0+((uint64_t)(h-1U)*b->width+w)*bytes_per_pixel(b);
  return a0<b1&&b0<a1; /* Conservative span check; never memmove semantics. */
}
uint32_t DG_IsBusy(void)
{return transaction_open||g_dma2d_diagnostic.async_inflight||(DMA2D->CR&DMA2D_CR_START)!=0U||HAL_DMA2D_GetState(&hdma2d)==HAL_DMA2D_STATE_BUSY;}
static void complete_callback(DMA2D_HandleTypeDef *handle)
{
  (void)handle;irq_cycles=DWT->CYCCNT;
  if(g_dma2d_diagnostic.state!=LCD_STATE_FAIL) {
    g_dma2d_diagnostic.completed_count++;g_dma2d_diagnostic.pixels_processed+=pending_pixels;
  }
  g_dma2d_diagnostic.async_inflight=0U;__DMB();irq_done=1U;
}
static void error_callback(DMA2D_HandleTypeDef *handle)
{
  g_dma2d_diagnostic.last_hal_error=HAL_DMA2D_GetError(handle);
  g_dma2d_diagnostic.last_hal_status=HAL_ERROR;DG_Fail(DG_HAL_ERROR);
}
DG_Status DG_Init(void)
{
  g_dma2d_diagnostic=(DG_Diagnostic){0};g_dma2d_diagnostic.state=LCD_STATE_RUNNING;
  g_dma2d_diagnostic.stage=1U;hdma2d.Instance=DMA2D;
  /* RM0090 6.3.10,11.3.11: HAL MSP enables only DMA2D clock/NVIC, no FMC/GPIO. */
  hdma2d.Init.Mode=DMA2D_R2M;hdma2d.Init.ColorMode=DMA2D_OUTPUT_RGB565;hdma2d.Init.OutputOffset=0U;
  HAL_StatusTypeDef h=HAL_DMA2D_Init(&hdma2d);g_dma2d_diagnostic.last_hal_status=h;
  if(h!=HAL_OK) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  /* CMSIS Cortex-M4 DWT/DEMCR; count short intervals modulo 2^32 (<100 ms). */
  CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
  if((DWT->CTRL&DWT_CTRL_NOCYCCNT_Msk)!=0U) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;__DSB();__ISB();
  uint32_t a=DWT->CYCCNT;for(volatile uint32_t i=0U;i<100U;i++) {__NOP();}
  g_dma2d_diagnostic.dwt_probe_cycles=DWT->CYCCNT-a;
  g_dma2d_diagnostic.dwt_ctrl=DWT->CTRL;g_dma2d_diagnostic.nominal_hclk_hz=SystemCoreClock;
  if(g_dma2d_diagnostic.dwt_probe_cycles==0U||DG_IsBusy()) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  DG_Registers r;DG_Snapshot(&r);g_dma2d_diagnostic.before=r;return DG_OK;
}
DG_Status DG_Start(const DG_Job *j,uint32_t interrupt_mode)
{
  uint32_t entry_cycles=DWT->CYCCNT;
  if(g_dma2d_diagnostic.state==LCD_STATE_FAIL) return DG_HAL_ERROR;
  if(DG_IsBusy()) {g_dma2d_diagnostic.busy_rejections++;return DG_BUSY;}
  if(j==NULL||j->operation>DG_BLEND||interrupt_mode>1U
      ||!rectangle_valid(j->dst,j->dx,j->dy,j->width,j->height)
      ||(j->operation==DG_FILL&&j->dst->format!=DMA2D_OUTPUT_RGB565)
      ||(j->operation!=DG_FILL&&(!rectangle_valid(j->fg,j->fx,j->fy,j->width,j->height)
        ||overlaps(j->dst,j->dx,j->dy,j->fg,j->fx,j->fy,j->width,j->height)))
      ||(j->operation==DG_COPY&&j->dst->format!=j->fg->format)
      ||(j->operation==DG_BLEND&&(!rectangle_valid(j->bg,j->bx,j->by,j->width,j->height)
        ||overlaps(j->dst,j->dx,j->dy,j->bg,j->bx,j->by,j->width,j->height)))) {
    g_dma2d_diagnostic.invalid_rejections++;return DG_INVALID;
  }
  call_cycles=entry_cycles;
  const uint32_t mode=j->operation==DG_FILL?DMA2D_R2M:j->operation==DG_COPY?DMA2D_M2M:
    j->operation==DG_PFC?DMA2D_M2M_PFC:DMA2D_M2M_BLEND;
  /* RM0090 11.3.11 / 11.5: offsets measured in pixels, not bytes. */
  hdma2d.Init.Mode=mode;hdma2d.Init.ColorMode=j->dst->format;hdma2d.Init.OutputOffset=j->dst->width-j->width;
  HAL_StatusTypeDef h=HAL_DMA2D_Init(&hdma2d);
  if(j->operation!=DG_FILL&&h==HAL_OK) {
    hdma2d.LayerCfg[1].InputColorMode=j->fg->format;hdma2d.LayerCfg[1].InputOffset=j->fg->width-j->width;
    hdma2d.LayerCfg[1].AlphaMode=DMA2D_NO_MODIF_ALPHA;hdma2d.LayerCfg[1].InputAlpha=255U;
    h=HAL_DMA2D_ConfigLayer(&hdma2d,1U);
  }
  if(j->operation==DG_BLEND&&h==HAL_OK) {
    hdma2d.LayerCfg[0].InputColorMode=j->bg->format;hdma2d.LayerCfg[0].InputOffset=j->bg->width-j->width;
    hdma2d.LayerCfg[0].AlphaMode=DMA2D_NO_MODIF_ALPHA;hdma2d.LayerCfg[0].InputAlpha=255U;
    h=HAL_DMA2D_ConfigLayer(&hdma2d,0U);
  }
  if(h!=HAL_OK) {g_dma2d_diagnostic.last_hal_status=h;DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  hdma2d.XferCpltCallback=complete_callback;hdma2d.XferErrorCallback=error_callback;
  uint32_t flags=DMA2D->ISR;g_dma2d_diagnostic.last_dma2d_isr=flags;
  if(flags&(DMA2D_ISR_TEIF|DMA2D_ISR_CEIF|DMA2D_ISR_CAEIF)) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  DMA2D->IFCR=DMA2D_IFCR_CTCIF|DMA2D_IFCR_CCTCIF;
  uint32_t src=j->operation==DG_FILL?DG_Expand565(j->color):address(j->fg,j->fx,j->fy);
  if(j->operation==DG_FILL&&j->dst->format!=DMA2D_OUTPUT_RGB565) {
    g_dma2d_diagnostic.invalid_rejections++;return DG_INVALID;
  }
  g_dma2d_diagnostic.current_mode=j->operation;
  g_dma2d_diagnostic.source_addr=j->operation==DG_FILL?0U:src;
  g_dma2d_diagnostic.background_addr=j->operation==DG_BLEND?address(j->bg,j->bx,j->by):0U;
  g_dma2d_diagnostic.destination_addr=address(j->dst,j->dx,j->dy);
  pending_pixels=j->width*j->height;irq_done=0U;transaction_open=1U;active_irq_mode=interrupt_mode;
  g_dma2d_diagnostic.async_inflight=interrupt_mode;
  __DSB();start_tick_ms=HAL_GetTick();start_cycles=DWT->CYCCNT;
  g_dma2d_diagnostic.setup_cycles=start_cycles-call_cycles;
  if(j->operation==DG_BLEND) {
    h=interrupt_mode?HAL_DMA2D_BlendingStart_IT(&hdma2d,src,g_dma2d_diagnostic.background_addr,
      g_dma2d_diagnostic.destination_addr,j->width,j->height):HAL_DMA2D_BlendingStart(&hdma2d,src,
      g_dma2d_diagnostic.background_addr,g_dma2d_diagnostic.destination_addr,j->width,j->height);
  } else {
    h=interrupt_mode?HAL_DMA2D_Start_IT(&hdma2d,src,g_dma2d_diagnostic.destination_addr,j->width,j->height):
      HAL_DMA2D_Start(&hdma2d,src,g_dma2d_diagnostic.destination_addr,j->width,j->height);
  }
  uint32_t returned=DWT->CYCCNT;g_dma2d_diagnostic.launch_cycles=returned-start_cycles;
  g_dma2d_diagnostic.last_hal_status=h;g_dma2d_diagnostic.last_hal_error=HAL_DMA2D_GetError(&hdma2d);
  if(h!=HAL_OK) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  g_dma2d_diagnostic.transfer_count++;start_cycles=returned;return DG_OK;
}
DG_Status DG_Wait(uint32_t timeout_ms)
{
  if(!transaction_open) return DG_INVALID;
  for(;;) {
    uint32_t isr=DMA2D->ISR;g_dma2d_diagnostic.last_dma2d_isr=isr;
    if(isr&(DMA2D_ISR_TEIF|DMA2D_ISR_CEIF|DMA2D_ISR_CAEIF)) {
      g_dma2d_diagnostic.dma2d_error_count++;DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;
    }
    if(g_dma2d_diagnostic.state==LCD_STATE_FAIL) return DG_HAL_ERROR;
    if(active_irq_mode?irq_done:(isr&DMA2D_ISR_TCIF)!=0U) break;
    if(HAL_GetTick()-start_tick_ms>timeout_ms||DWT->CYCCNT-start_cycles>SystemCoreClock/10U) {
      g_dma2d_diagnostic.timeout_count++;DG_Fail(DG_TIMEOUT);return DG_TIMEOUT;
    }
    if(g_lcd_diagnostic.state==LCD_STATE_FAIL) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  }
  uint32_t end=active_irq_mode?irq_cycles:DWT->CYCCNT;
  /* PollForTransfer is called only after status captured and TC observed;
   * it cannot erase an error before our snapshot or wait forever on a dead tick. */
  HAL_StatusTypeDef h=active_irq_mode?HAL_OK:HAL_DMA2D_PollForTransfer(&hdma2d,0U);
  __DSB();g_dma2d_diagnostic.tail_cycles=end-start_cycles;
  if(active_irq_mode&&(int32_t)(end-start_cycles)<0) g_dma2d_diagnostic.tail_cycles=0U;
  g_dma2d_diagnostic.total_cycles=DWT->CYCCNT-call_cycles;
  g_dma2d_diagnostic.last_hal_status=h;g_dma2d_diagnostic.last_hal_error=HAL_DMA2D_GetError(&hdma2d);
  if(h!=HAL_OK) {DG_Fail(DG_HAL_ERROR);return DG_HAL_ERROR;}
  if(!active_irq_mode) {g_dma2d_diagnostic.completed_count++;g_dma2d_diagnostic.pixels_processed+=pending_pixels;}
  transaction_open=0U;return DG_OK;
}
void DG_IRQHandler(void)
{
  /* RM0090 11.4/11.5.2-3: save ISR/error registers before HAL clears W1C flags.
   * Short SRAM/AHB-slave operations only; no rendering, waits or FMC accesses. */
  uint32_t isr=DMA2D->ISR;g_dma2d_diagnostic.last_dma2d_isr=isr;
  g_dma2d_diagnostic.irq_count++;
  if(isr&(DMA2D_ISR_TEIF|DMA2D_ISR_CEIF|DMA2D_ISR_CAEIF)) {
    g_dma2d_diagnostic.dma2d_error_count++;DG_Fail(DG_HAL_ERROR);
  }
  HAL_DMA2D_IRQHandler(&hdma2d);
}
void DG_RecordHardFault(void)
{
  g_dma2d_diagnostic.hardfault_count++;g_dma2d_diagnostic.cfsr=SCB->CFSR;
  g_dma2d_diagnostic.hfsr=SCB->HFSR;g_dma2d_diagnostic.bfar_valid=(SCB->CFSR&SCB_CFSR_BFARVALID_Msk)!=0U;
  if(g_dma2d_diagnostic.bfar_valid) g_dma2d_diagnostic.bfar=SCB->BFAR;
  DG_Fail(DG_HARDFAULT);__DSB();
}
__attribute__((noinline,noreturn)) void DG_DiagnosticStop(void) {for(;;) __WFI();}
__attribute__((noinline)) void DG_Checkpoint(void) {__NOP();}
