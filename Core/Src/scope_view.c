#include "scope_view.h"
#include "dma2d_graphics.h"
#include <string.h>

#define SV_WIDTH_PIXELS 240U
#define SV_HEIGHT_PIXELS 320U
#define SV_TOP_PIXELS 8U
#define SV_BOTTOM_PIXELS 311U
#define SV_ADC_FULL_SCALE_CODE 4095U
#define SV_GRAY_RGB565 0x4208U
#define SV_GREEN_RGB565 0x07E0U
#define SV_OPT __attribute__((optimize("O2")))
volatile SV_Diagnostic g_scope_diagnostic;
/* Columns are owned by main; no ISR or DMA writes them. ADC snapshot may
 * advance while DMA2D clears the framebuffer, without changing this frame. */
static uint16_t column_min[SV_WIDTH_PIXELS],column_max[SV_WIDTH_PIXELS];
static uint32_t next_frame_tick_ms;

static SV_OPT uint32_t ordinate(uint32_t code)
{
 return SV_BOTTOM_PIXELS-code*(SV_BOTTOM_PIXELS-SV_TOP_PIXELS)/SV_ADC_FULL_SCALE_CODE;
}
static SV_OPT void pixel(uint32_t x,uint32_t y,uint16_t color)
{
 if(x<SV_WIDTH_PIXELS&&y<SV_HEIGHT_PIXELS)
  ((volatile uint16_t *)LCD_FRAMEBUFFER_ADDRESS)[y*SV_WIDTH_PIXELS+x]=color;
}
static SV_OPT void render(void)
{
 uint32_t begin=DWT->CYCCNT;
 for(uint32_t i=0;i<=10U;i++) {
  uint32_t x=i*(SV_WIDTH_PIXELS-1U)/10U;
  for(uint32_t y=SV_TOP_PIXELS;y<=SV_BOTTOM_PIXELS;y++)pixel(x,y,SV_GRAY_RGB565);
 }
 for(uint32_t i=0;i<=8U;i++) {
  uint32_t y=SV_TOP_PIXELS+i*(SV_BOTTOM_PIXELS-SV_TOP_PIXELS)/8U;
  for(uint32_t x=0;x<SV_WIDTH_PIXELS;x++)pixel(x,y,SV_GRAY_RGB565);
 }
 uint32_t previous=ordinate((column_min[0]+column_max[0])/2U);
 for(uint32_t x=0;x<SV_WIDTH_PIXELS;x++) {
  uint32_t top=ordinate(column_max[x]),bottom=ordinate(column_min[x]);
  for(uint32_t y=top;y<=bottom;y++)pixel(x,y,SV_GREEN_RGB565);
  /* Join adjacent column centres, retaining each column's extrema as well. */
  uint32_t centre=ordinate((column_min[x]+column_max[x])/2U);
  if(x) {
   uint32_t lo=centre<previous?centre:previous,hi=centre>previous?centre:previous;
   for(uint32_t y=lo;y<=hi;y++)pixel(x,y,SV_GREEN_RGB565);
  }
  previous=centre;
 }
 __DSB();
 uint32_t cycles=DWT->CYCCNT-begin;
 if(cycles>g_scope_diagnostic.render_max_cycles)g_scope_diagnostic.render_max_cycles=cycles;
 g_scope_diagnostic.frames++;g_adc_diagnostic.display_frames++;
}
void SV_Init(void)
{
 memset((void *)&g_scope_diagnostic,0,sizeof(g_scope_diagnostic));
 g_scope_diagnostic.frame_period_tick_ms=SV_FRAME_PERIOD_TICK_MS;
 g_scope_diagnostic.window_samples=AC_BLOCK_SAMPLES;
 next_frame_tick_ms=HAL_GetTick();
}
uint32_t SV_IsIdle(void){return !g_scope_diagnostic.clear_pending;}
SV_OPT void SV_Process(void)
{
 uint32_t now=HAL_GetTick();
 if(g_scope_diagnostic.clear_pending) {
  if(g_dma2d_diagnostic.state==LCD_STATE_FAIL||now-g_scope_diagnostic.clear_start_tick_ms>100U)
   {AC_Fail(AC_GRAPHICS);return;}
  /* RM0090 11.3.11/11.4: CPU must not write framebuffer while R2M runs.
   * The existing callback publishes completion; DG_Wait then closes its
   * transaction immediately. Never wait for hardware in the ADC service loop. */
  if(g_dma2d_diagnostic.async_inflight)return;
  if(DG_Wait(100U)!=DG_OK){AC_Fail(AC_GRAPHICS);return;}
  g_scope_diagnostic.clear_pending=0U;render();return;
 }
 if((int32_t)(now-next_frame_tick_ms)<0||g_adc_diagnostic.snapshot_samples!=AC_BLOCK_SAMPLES||
    g_scope_diagnostic.frame_generation==g_adc_diagnostic.snapshot_generation)return;
 for(uint32_t x=0;x<SV_WIDTH_PIXELS;x++) {
  uint32_t first=x*AC_BLOCK_SAMPLES/SV_WIDTH_PIXELS;
  uint32_t end=(x+1U)*AC_BLOCK_SAMPLES/SV_WIDTH_PIXELS,lo=4095U,hi=0U;
  for(uint32_t i=first;i<end;i++) {
   uint32_t v=adc_snapshot[i];if(v<lo)lo=v;if(v>hi)hi=v;
  }
  column_min[x]=(uint16_t)lo;column_max[x]=(uint16_t)hi;
 }
 g_scope_diagnostic.frame_generation=g_adc_diagnostic.snapshot_generation;
 DG_Job fill={.operation=DG_FILL,.dst=&dg_framebuffer,
  .width=SV_WIDTH_PIXELS,.height=SV_HEIGHT_PIXELS,.color=0U};
 if(DG_Start(&fill,1U)!=DG_OK){AC_Fail(AC_GRAPHICS);return;}
 g_scope_diagnostic.clear_pending=1U;g_scope_diagnostic.clear_start_tick_ms=now;
 next_frame_tick_ms=now+SV_FRAME_PERIOD_TICK_MS;
}
