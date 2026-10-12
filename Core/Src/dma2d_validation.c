#include "dma2d_graphics.h"
#define GUARD_HALFWORDS 16U
static const DG_Surface * const buffers[]={&dg_scratch_a,&dg_scratch_b,&dg_argb_source,&dg_argb_destination};
_Static_assert(0xD0025800U+32U<0xD0100000U,"Framebuffer / CPU test");
_Static_assert(0xD0101000U+32U<0xD0200000U-32U,"CPU test / scratch A");
_Static_assert(0xD0225800U+32U<0xD0240000U-32U,"Scratch A / B");
_Static_assert(0xD0265800U+32U<0xD0300000U-32U,"Scratch B / ARGB source");
_Static_assert(0xD0304000U+32U<0xD0310000U-32U,"ARGB source / destination");
_Static_assert(0xD0314000U+32U<=SDRAM_BASE_ADDRESS+SDRAM_SIZE_BYTES,"All regions inside SDRAM");
static uint32_t size_bytes(const DG_Surface *s) {return s->width*s->height*(s->format==2U?2U:4U);}
static void mismatch(uint32_t address,uint32_t expected,uint32_t actual)
{
  if(!g_dma2d_diagnostic.verification_errors) {
    g_dma2d_diagnostic.first_error_addr=address;g_dma2d_diagnostic.expected=expected;g_dma2d_diagnostic.actual=actual;
  }
  g_dma2d_diagnostic.verification_errors++;DG_Fail(DG_VERIFY);
}
static void guards_init(void)
{
  for(uint32_t b=0U;b<4U;b++) for(uint32_t i=0U;i<GUARD_HALFWORDS;i++) {
    ((volatile uint16_t *)(uintptr_t)(buffers[b]->address-32U))[i]=(uint16_t)(0xA500U+b*32U+i);
    ((volatile uint16_t *)(uintptr_t)(buffers[b]->address+size_bytes(buffers[b])))[i]=(uint16_t)(0x5A00U+b*32U+i);
  }
  static const uint32_t extra[]={0xD0025800U,0xD00FFFE0U,0xD0101000U};
  for(uint32_t b=0U;b<3U;b++) for(uint32_t i=0U;i<GUARD_HALFWORDS;i++)
    ((volatile uint16_t *)(uintptr_t)extra[b])[i]=(uint16_t)(0xC300U+b*32U+i);
  __DSB();
}
static void guards_check(void)
{
  for(uint32_t b=0U;b<4U;b++) for(uint32_t i=0U;i<GUARD_HALFWORDS;i++) {
    uint32_t addresses[]={buffers[b]->address-32U+i*2U,buffers[b]->address+size_bytes(buffers[b])+i*2U};
    for(uint32_t k=0U;k<2U;k++) {
      uint16_t e=(uint16_t)((k?0x5A00U:0xA500U)+b*32U+i);
      uint16_t a=*(volatile uint16_t *)(uintptr_t)addresses[k];
      if(e!=a) mismatch(addresses[k],e,a);
      g_dma2d_diagnostic.guard_checks++;
    }
  }
  static const uint32_t extra[]={0xD0025800U,0xD00FFFE0U,0xD0101000U};
  for(uint32_t b=0U;b<3U;b++) for(uint32_t i=0U;i<GUARD_HALFWORDS;i++) {
    uint32_t addr=extra[b]+i*2U;uint16_t e=(uint16_t)(0xC300U+b*32U+i),a=*(volatile uint16_t *)(uintptr_t)addr;
    if(a!=e) mismatch(addr,e,a);
    g_dma2d_diagnostic.guard_checks++;
  }
}
static void fill_cpu(const DG_Surface *s,uint16_t color)
{volatile uint16_t *p=(volatile uint16_t *)(uintptr_t)s->address;for(uint32_t i=0U;i<s->width*s->height;i++) p[i]=color;__DSB();}
static void verify_fill(const DG_Job *j,uint16_t outside)
{
  for(uint32_t y=0U;y<j->dst->height;y++) for(uint32_t x=0U;x<j->dst->width;x++) {
    uint16_t e=x>=j->dx&&x-j->dx<j->width&&y>=j->dy&&y-j->dy<j->height?j->color:outside;
    uint32_t a=j->dst->address+2U*(y*j->dst->width+x);
    uint16_t v=*(volatile uint16_t *)(uintptr_t)a;g_dma2d_diagnostic.checked_pixels++;
    if(v!=e) mismatch(a,e,v);
  }
  guards_check();if(g_dma2d_diagnostic.state==LCD_STATE_FAIL) DG_DiagnosticStop();
  g_dma2d_diagnostic.tests_passed++;
}
static void execute(const DG_Job *j,uint32_t irq)
{if(DG_Start(j,irq)!=DG_OK||DG_Wait(50U)!=DG_OK) DG_DiagnosticStop();}
static void fill_tests(void)
{
  g_dma2d_diagnostic.stage=2U;
  static const uint16_t colors[]={0x0000U,0xFFFFU,0xF800U,0x07E0U,0x001FU,0xFFE0U,0xF81FU,0x07FFU};
  DG_Job j={.operation=DG_FILL,.dst=&dg_scratch_a,.width=240U,.height=320U};
  for(uint32_t c=0U;c<8U;c++) {j.color=colors[c];execute(&j,0U);verify_fill(&j,0U);}
  g_dma2d_diagnostic.stage_pass_mask|=1U<<2;DG_Checkpoint();
  g_dma2d_diagnostic.stage=3U;
  static const uint16_t sizes[][2]={{1,1},{8,8},{16,16},{64,64},{120,160},{240,320},{7,13},{37,29}};
  for(uint32_t n=0U;n<8U;n++) for(uint32_t p=0U;p<5U;p++) {
    j.width=sizes[n][0];j.height=sizes[n][1];j.color=0x39E7U;
    j.dx=p==0U?0U:p==1U?(240U-j.width)/2U:p==4U?0U:240U-j.width;
    j.dy=p==0U?0U:p==1U?(320U-j.height)/2U:p==3U?0U:320U-j.height;
    fill_cpu(&dg_scratch_a,0xA55AU);execute(&j,0U);verify_fill(&j,0xA55AU);
  }
  g_dma2d_diagnostic.stage_pass_mask|=1U<<3;DG_Checkpoint();
}
static uint32_t pattern_tag;
static uint16_t pattern565(uint32_t x,uint32_t y)
{
  static const uint16_t c[]={0x0000U,0xFFFFU,0xF800U,0x07E0U,0x001FU,0xFFE0U,0xF81FU,0x07FFU};
  if(pattern_tag==0U) return (uint16_t)((y*240U+x)*0x9E37U^((y*240U+x)>>7)^0x1357U);
  if(pattern_tag==3U) return 0x001FU;
  if((x&15U)<8U) return c[x&7U];
  return (uint16_t)(((x&31U)<<11)|((y&63U)<<5)|((x+y)&31U));
}
static uint32_t pattern8888(uint32_t x,uint32_t y)
{
  if(pattern_tag==3U) {
    if(y<16U) return 0x00FF0000U;
    if(y<32U) return 0x8000FF00U;
    if(y<48U) return 0xFFFFFFFFU;
    return ((x*255U/63U)<<24)|0x00FF0000U|((y*4U&255U)<<8)|0x35U;
  }
  return ((x*255U/63U)<<24)|((x*4U)<<16)|((y*4U)<<8)|((x*13U+y*7U)&255U);
}
static void prepare_sources(uint32_t tag)
{
  pattern_tag=tag;
  volatile uint16_t *a=(volatile uint16_t *)(uintptr_t)dg_scratch_a.address;
  volatile uint32_t *f=(volatile uint32_t *)(uintptr_t)dg_argb_source.address;
  for(uint32_t y=0U;y<320U;y++) for(uint32_t x=0U;x<240U;x++) a[y*240U+x]=pattern565(x,y);
  for(uint32_t y=0U;y<64U;y++) for(uint32_t x=0U;x<64U;x++) f[y*64U+x]=pattern8888(x,y);
  __DSB();
}
static void prepare_dest(const DG_Surface *s)
{
  if(s->format==2U) fill_cpu(s,0xA55AU);
  else {volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)s->address;for(uint32_t i=0U;i<4096U;i++) p[i]=0x1A2B3C4DU;__DSB();}
}
static uint32_t read_pixel(const DG_Surface *s,uint32_t x,uint32_t y)
{
  uint32_t index=y*s->width+x;
  return s->format==2U?((volatile uint16_t *)(uintptr_t)s->address)[index]:((volatile uint32_t *)(uintptr_t)s->address)[index];
}
static uint32_t expected_pixel(const DG_Job *j,uint32_t x,uint32_t y)
{
  if(x<j->dx||x-j->dx>=j->width||y<j->dy||y-j->dy>=j->height) return j->dst->format==2U?0xA55AU:0x1A2B3C4DU;
  if(j->operation==DG_FILL) return j->color;
  uint32_t fx=x-j->dx+j->fx,fy=y-j->dy+j->fy;
  if(j->operation==DG_COPY) return pattern565(fx,fy);
  if(j->operation==DG_PFC) return j->fg->format==2U?DG_Expand565(pattern565(fx,fy)):DG_Quantize565(pattern8888(fx,fy));
  return DG_Blend565(pattern8888(fx,fy),pattern565(x-j->dx+j->bx,y-j->dy+j->by));
}
static uint32_t blend_close(uint16_t e,uint16_t a,uint32_t alpha)
{
  if(alpha==0U||alpha==255U) return e==a;
  uint32_t shifts[]={11U,5U,0U},masks[]={31U,63U,31U};
  for(uint32_t k=0U;k<3U;k++) {
    int d=(int)((e>>shifts[k])&masks[k])-(int)((a>>shifts[k])&masks[k]);
    uint32_t delta=(uint32_t)(d<0?-d:d);
    if(delta>g_dma2d_diagnostic.blend_max_channel_difference) g_dma2d_diagnostic.blend_max_channel_difference=delta;
    if(delta>1U) return 0U;
  }
  if(e!=a) g_dma2d_diagnostic.blend_rounding_pixels++;
  return 1U;
}
static void verify_job(const DG_Job *j)
{
  for(uint32_t y=0U;y<j->dst->height;y++) for(uint32_t x=0U;x<j->dst->width;x++) {
    uint32_t e=expected_pixel(j,x,y),a=read_pixel(j->dst,x,y);g_dma2d_diagnostic.checked_pixels++;
    uint32_t ok=e==a;
    if(j->operation==DG_BLEND&&x>=j->dx&&x-j->dx<j->width&&y>=j->dy&&y-j->dy<j->height)
      ok=blend_close((uint16_t)e,(uint16_t)a,pattern8888(x-j->dx+j->fx,y-j->dy+j->fy)>>24);
    if(!ok) mismatch(j->dst->address+(y*j->dst->width+x)*(j->dst->format==2U?2U:4U),e,a);
  }
  /* Whole source buffers are checked against their generators, not copied RAM. */
  for(uint32_t y=0U;y<320U;y++) for(uint32_t x=0U;x<240U;x++) {
    uint32_t e=pattern565(x,y),a=read_pixel(&dg_scratch_a,x,y);
    if(e!=a) mismatch(dg_scratch_a.address+2U*(y*240U+x),e,a);
  }
  for(uint32_t y=0U;y<64U;y++) for(uint32_t x=0U;x<64U;x++) {
    uint32_t e=pattern8888(x,y),a=read_pixel(&dg_argb_source,x,y);
    if(e!=a) mismatch(dg_argb_source.address+4U*(y*64U+x),e,a);
  }
  guards_check();if(g_dma2d_diagnostic.state==LCD_STATE_FAIL) DG_DiagnosticStop();
  g_dma2d_diagnostic.tests_passed++;
}
static void copy_pfc_blend_tests(void)
{
  g_dma2d_diagnostic.stage=4U;prepare_sources(0U);
  static const uint16_t sizes[][2]={{240,320},{8,8},{16,16},{64,64},{120,80},{239,1},{1,319}};
  DG_Job j={.operation=DG_COPY,.fg=&dg_scratch_a,.dst=&dg_scratch_b};
  for(uint32_t k=0U;k<7U;k++) {
    j.width=sizes[k][0];j.height=sizes[k][1];
    j.fx=(240U-j.width)/3U;j.fy=(320U-j.height)/4U;
    j.dx=240U-j.width;j.dy=320U-j.height;
    prepare_dest(j.dst);execute(&j,0U);verify_job(&j);
  }
  g_dma2d_diagnostic.stage_pass_mask|=1U<<4;DG_Checkpoint();
  g_dma2d_diagnostic.stage=5U;prepare_sources(1U);
  j=(DG_Job){.operation=DG_PFC,.fg=&dg_scratch_a,.dst=&dg_argb_destination,
    .fx=73U,.fy=117U,.width=64U,.height=64U};
  prepare_dest(j.dst);execute(&j,0U);verify_job(&j);
  j=(DG_Job){.operation=DG_PFC,.fg=&dg_argb_source,.dst=&dg_scratch_b,
    .dx=13U,.dy=41U,.width=64U,.height=64U};
  prepare_dest(j.dst);execute(&j,0U);verify_job(&j);
  g_dma2d_diagnostic.stage_pass_mask|=1U<<5;DG_Checkpoint();
  g_dma2d_diagnostic.stage=6U;prepare_sources(3U);
  j=(DG_Job){.operation=DG_BLEND,.fg=&dg_argb_source,.bg=&dg_scratch_a,.dst=&dg_scratch_b,
    .bx=37U,.by=49U,.dx=131U,.dy=213U,.width=64U,.height=64U};
  prepare_dest(j.dst);execute(&j,0U);verify_job(&j);
  g_dma2d_diagnostic.stage_pass_mask|=1U<<6;DG_Checkpoint();
}
static void irq_tests(void)
{
  g_dma2d_diagnostic.stage=7U;
  DG_Job j={.operation=DG_FILL,.dst=&dg_scratch_b,.width=240U,.height=320U,.color=0xF81FU};
  prepare_dest(j.dst);
  if(DG_Start(&j,1U)!=DG_OK) DG_DiagnosticStop();
  if(g_dma2d_diagnostic.async_inflight&&(DMA2D->CR&DMA2D_CR_START)) g_dma2d_diagnostic.async_returned_pending++;
  if(DG_Start(&j,1U)!=DG_BUSY) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  if(DG_Wait(50U)!=DG_OK) DG_DiagnosticStop();
  verify_fill(&j,0xA55AU);
  prepare_sources(0U);
  j=(DG_Job){.operation=DG_COPY,.fg=&dg_scratch_a,.dst=&dg_scratch_b,.width=240U,.height=320U};
  prepare_dest(j.dst);execute(&j,1U);verify_job(&j);
  /* Negative API tests reject invalid jobs before touching peripheral registers. */
  DG_Registers before,after;DG_Snapshot(&before);
  static const uint32_t bad_widths[]={0U,241U,0xFFFFFFFFU};
  for(uint32_t i=0U;i<3U;i++) {
    j.width=bad_widths[i];if(DG_Start(&j,0U)!=DG_INVALID) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  }
  j.width=240U;j.fg=&dg_scratch_b;
  if(DG_Start(&j,0U)!=DG_INVALID) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  DG_Surface ccm={0x10000000U,240U,320U,2U};j.fg=&ccm;
  if(DG_Start(&j,0U)!=DG_INVALID) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  DG_Snapshot(&after);
  const uint32_t *a=(const uint32_t *)&before,*b=(const uint32_t *)&after;
  for(uint32_t i=0U;i<sizeof(before)/4U;i++) if(a[i]!=b[i]) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  if(g_dma2d_diagnostic.irq_count<2U||!g_dma2d_diagnostic.async_returned_pending) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  g_dma2d_diagnostic.stage_pass_mask|=1U<<7;DG_Checkpoint();
}
static uint32_t display_start_tick_ms,display_last_animation_tick_ms,display_x_pixels;
static void snapshot_configuration(uint32_t before)
{
  /* RM0090 37.7.5,16.7: status captured, only configuration requires invariance. */
  SDRAM_RegisterSnapshot f={.sdcr1=FMC_Bank5_6->SDCR[0],.sdcr2=FMC_Bank5_6->SDCR[1],
    .sdtr1=FMC_Bank5_6->SDTR[0],.sdtr2=FMC_Bank5_6->SDTR[1],.sdrtr=FMC_Bank5_6->SDRTR,.sdsr=FMC_Bank5_6->SDSR};
  LCD_RegisterSnapshot l={.sscr=LTDC->SSCR,.bpcr=LTDC->BPCR,.awcr=LTDC->AWCR,.twcr=LTDC->TWCR,
    .gcr=LTDC->GCR,.srcr=LTDC->SRCR,.ier=LTDC->IER,.isr=LTDC->ISR,.cpsr=LTDC->CPSR,.cdsr=LTDC->CDSR,
    .l1cr=LTDC_Layer1->CR,.l1whpcr=LTDC_Layer1->WHPCR,.l1wvpcr=LTDC_Layer1->WVPCR,
    .l1pfcr=LTDC_Layer1->PFCR,.l1cacr=LTDC_Layer1->CACR,.l1bfcr=LTDC_Layer1->BFCR,
    .l1cfbar=LTDC_Layer1->CFBAR,.l1cfblr=LTDC_Layer1->CFBLR,.l1cfblnr=LTDC_Layer1->CFBLNR,.l2cr=LTDC_Layer2->CR};
  if(before) {g_dma2d_diagnostic.fmc_before=f;g_dma2d_diagnostic.ltdc_before=l;}
  else {
    g_dma2d_diagnostic.fmc_after=f;g_dma2d_diagnostic.ltdc_after=l;
    const volatile SDRAM_RegisterSnapshot *a=&g_dma2d_diagnostic.fmc_before;
    g_dma2d_diagnostic.fmc_unchanged=f.sdcr1==a->sdcr1&&f.sdcr2==a->sdcr2&&f.sdtr1==a->sdtr1&&f.sdtr2==a->sdtr2&&f.sdrtr==a->sdrtr;
    const volatile LCD_RegisterSnapshot *b=&g_dma2d_diagnostic.ltdc_before;
    if(!g_dma2d_diagnostic.fmc_unchanged||l.sscr!=b->sscr||l.bpcr!=b->bpcr||l.awcr!=b->awcr||l.twcr!=b->twcr
       ||l.gcr!=b->gcr||l.ier!=b->ier||l.l1cr!=b->l1cr||l.l1whpcr!=b->l1whpcr||l.l1wvpcr!=b->l1wvpcr
       ||l.l1pfcr!=b->l1pfcr||l.l1cacr!=b->l1cacr||l.l1bfcr!=b->l1bfcr||l.l1cfbar!=b->l1cfbar
       ||l.l1cfblr!=b->l1cfblr||l.l1cfblnr!=b->l1cfblnr||l.l2cr!=b->l2cr) DG_Fail(DG_CONFIG_CHANGED);
  }
  g_dma2d_diagnostic.ltdc_fifo_errors=g_lcd_diagnostic.fifo_underrun_count;
  g_dma2d_diagnostic.ltdc_transfer_errors=g_lcd_diagnostic.transfer_error_count;
}
static uint16_t copy_image_pixel(uint32_t x,uint32_t y)
{
  if(x<16U&&y<16U) return 0xF800U;
  if(x>=224U&&y<16U) return 0x07E0U;
  if(x<16U&&y>=304U) return 0x001FU;
  if(x>=224U&&y>=304U) return 0xFFE0U;
  if(x==0U||x==239U||y==0U||y==319U) return 0xFFFFU;
  return ((x/8U+y/8U)&1U)?0xFFFFU:0x0000U;
}
static void screen_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint16_t color)
{
  DG_Job j={.operation=DG_FILL,.dst=&dg_framebuffer,.dx=x,.dy=y,.width=w,.height=h,.color=color};execute(&j,0U);
}
static void display_image(uint32_t image)
{
  static const uint16_t colors[]={0U,0xFFFFU,0xF800U,0x07E0U,0x001FU};
  static const uint16_t bars[]={0xF800U,0x07E0U,0x001FU,0xFFE0U,0x07FFU,0xF81FU};
  g_dma2d_diagnostic.display_phase=image;
  if(image<5U) screen_rect(0U,0U,240U,320U,colors[image]);
  else if(image==5U) {
    for(uint32_t i=0U;i<6U;i++) screen_rect(i*40U,0U,40U,320U,bars[i]);
  } else if(image==6U||image==9U) {
    screen_rect(0U,0U,240U,320U,0U);
    for(uint32_t x=24U;x<240U;x+=24U) screen_rect(x,0U,1U,320U,0x4208U);
    for(uint32_t y=32U;y<320U;y+=32U) screen_rect(0U,y,240U,1U,0x4208U);
    screen_rect(0U,0U,240U,1U,0xFFFFU);screen_rect(0U,319U,240U,1U,0xFFFFU);
    screen_rect(0U,0U,1U,320U,0xFFFFU);screen_rect(239U,0U,1U,320U,0xFFFFU);
    if(image==6U) {
      screen_rect(3U,3U,7U,13U,0xF800U);screen_rect(101U,93U,37U,29U,0x07E0U);
      screen_rect(208U,272U,31U,47U,0x001FU);
    } else {
      screen_rect(1U,144U,238U,32U,0U);display_x_pixels=1U;
      screen_rect(display_x_pixels,152U,16U,16U,0xF81FU);
    }
  } else if(image==7U) {
    volatile uint16_t *a=(volatile uint16_t *)(uintptr_t)dg_scratch_a.address;
    for(uint32_t y=0U;y<320U;y++) for(uint32_t x=0U;x<240U;x++) a[y*240U+x]=copy_image_pixel(x,y);
    __DSB();DG_Job j={.operation=DG_COPY,.fg=&dg_scratch_a,.dst=&dg_framebuffer,.width=240U,.height=320U};execute(&j,0U);
  } else if(image==8U) {
    prepare_sources(3U);
    DG_Job j={.operation=DG_BLEND,.fg=&dg_argb_source,.bg=&dg_scratch_a,.dst=&dg_scratch_b,.width=64U,.height=64U};
    execute(&j,0U);screen_rect(0U,0U,240U,320U,0U);
    j=(DG_Job){.operation=DG_COPY,.fg=&dg_scratch_b,.dst=&dg_framebuffer,.dx=88U,.dy=128U,.width=64U,.height=64U};execute(&j,0U);
  }
  g_dma2d_diagnostic.stage=8U;
  display_start_tick_ms=HAL_GetTick();display_last_animation_tick_ms=display_start_tick_ms;
}
/* Equivalent useful pixel work, O2 only for this CPU baseline. Volatile keeps
 * real SDRAM accesses; no diagnostic write or pattern generation per pixel. */
__attribute__((optimize("O2"),noinline)) static void cpu_operation(const DG_Job *j)
{
  uint32_t out_stride=j->dst->width,fg_stride=j->fg?j->fg->width:0U,bg_stride=j->bg?j->bg->width:0U;
  volatile uint16_t *out16=(volatile uint16_t *)(uintptr_t)j->dst->address;
  volatile uint32_t *out32=(volatile uint32_t *)(uintptr_t)j->dst->address;
  volatile const uint16_t *fg16=(volatile const uint16_t *)(uintptr_t)(j->fg?j->fg->address:0U);
  volatile const uint32_t *fg32=(volatile const uint32_t *)(uintptr_t)(j->fg?j->fg->address:0U);
  volatile const uint16_t *bg16=(volatile const uint16_t *)(uintptr_t)(j->bg?j->bg->address:0U);
  for(uint32_t y=0U;y<j->height;y++) {
    uint32_t oi=(y+j->dy)*out_stride+j->dx,fi=(y+j->fy)*fg_stride+j->fx,bi=(y+j->by)*bg_stride+j->bx;
    if(j->operation==DG_FILL) {for(uint32_t x=0U;x<j->width;x++) out16[oi+x]=j->color;}
    else if(j->operation==DG_COPY) {for(uint32_t x=0U;x<j->width;x++) out16[oi+x]=fg16[fi+x];}
    else if(j->operation==DG_BLEND) {for(uint32_t x=0U;x<j->width;x++) out16[oi+x]=DG_Blend565(fg32[fi+x],bg16[bi+x]);}
    else if(j->dst->format==0U) {for(uint32_t x=0U;x<j->width;x++) out32[oi+x]=DG_Expand565(fg16[fi+x]);}
    else {for(uint32_t x=0U;x<j->width;x++) out16[oi+x]=DG_Quantize565(fg32[fi+x]);}
  }
  __DSB();
}
static void benchmarks(void)
{
  g_dma2d_diagnostic.stage=9U;
  static const uint16_t dimensions[][2]={{16,16},{64,64},{240,320},{64,64},{240,320},{64,64},{64,64},{64,64}};
  for(uint32_t b=0U;b<DMA2D_BENCH_COUNT;b++) {
    DG_Job j={.operation=b<3U?DG_FILL:b<5U?DG_COPY:b<7U?DG_PFC:DG_BLEND,
      .dst=b==5U?&dg_argb_destination:&dg_scratch_b,
      .fg=b>=6U?&dg_argb_source:&dg_scratch_a,.bg=&dg_scratch_a,
      .width=dimensions[b][0],.height=dimensions[b][1],.color=0x39E7U};
    if(b==5U) {j.fx=73U;j.fy=117U;}
    else if(j.width<240U) {j.dx=13U;j.dy=41U;}
    prepare_sources(b==7U?3U:1U);
    volatile DG_Benchmark *r=&g_dma2d_diagnostic.benchmarks[b];
    r->operation=j.operation;r->width=j.width;r->height=j.height;
    prepare_dest(j.dst);cpu_operation(&j);verify_job(&j);
    prepare_dest(j.dst);execute(&j,0U);verify_job(&j); /* Warm both paths. */
    for(uint32_t n=0U;n<DMA2D_BENCH_SAMPLES;n++) {
      prepare_dest(j.dst);__DSB();uint32_t t=DWT->CYCCNT;
      cpu_operation(&j);uint32_t cpu=DWT->CYCCNT-t;
      verify_job(&j); /* All verification outside timing. */
      prepare_dest(j.dst);__DSB();t=DWT->CYCCNT;
      execute(&j,0U);uint32_t total=DWT->CYCCNT-t;
      r->raw[n]=(DG_Sample){.cpu=cpu,.setup=g_dma2d_diagnostic.setup_cycles,
        .launch=g_dma2d_diagnostic.launch_cycles,.tail=g_dma2d_diagnostic.tail_cycles,.total=total};
      r->samples++;g_dma2d_diagnostic.cpu_cycles=cpu;g_dma2d_diagnostic.dma2d_cycles=total;
      verify_job(&j);
    }
  }
  g_dma2d_diagnostic.stage_pass_mask|=1U<<9;DG_Checkpoint();
}
static void verify_framebuffer(void)
{
  for(uint32_t y=0U;y<320U;y++) for(uint32_t x=0U;x<240U;x++) {
    uint16_t e=copy_image_pixel(x,y),a=(uint16_t)read_pixel(&dg_framebuffer,x,y);
    if(e!=a) mismatch(dg_framebuffer.address+2U*(y*240U+x),e,a);
  }
  g_dma2d_diagnostic.framebuffer_checks++;guards_check();
  if(g_dma2d_diagnostic.state==LCD_STATE_FAIL) DG_DiagnosticStop();
}
static void concurrent_load(void)
{
  g_dma2d_diagnostic.stage=10U;
  display_image(7U);g_dma2d_diagnostic.stage=10U;verify_framebuffer();
  /* J1: LTDC + DMA2D, without concurrent CPU writes. */
  DG_Job j={.operation=DG_FILL,.dst=&dg_scratch_a,.width=240U,.height=320U};
  for(uint32_t n=0U;n<64U;n++) {
    j.color=(uint16_t)(n*0x1357U);execute(&j,0U);
    volatile uint16_t *p=(volatile uint16_t *)(uintptr_t)j.dst->address;
    for(uint32_t i=0U;i<76800U;i++) if(p[i]!=j.color) mismatch(j.dst->address+i*2U,j.color,p[i]);
  }
  verify_framebuffer();snapshot_configuration(0U);
  if(g_dma2d_diagnostic.state==LCD_STATE_FAIL) DG_DiagnosticStop();
  g_dma2d_diagnostic.stage_pass_mask|=1U<<10;DG_Checkpoint();
  g_dma2d_diagnostic.stage=11U;
  g_dma2d_diagnostic.stress_start_tick_ms=HAL_GetTick();
  volatile uint16_t *cpu=(volatile uint16_t *)(uintptr_t)LCD_SDRAM_TEST_ADDRESS;
  do {
    uint32_t n=g_dma2d_diagnostic.stress_transfers;
    if(n&1U) j=(DG_Job){.operation=DG_COPY,.fg=&dg_scratch_a,.dst=&dg_scratch_b,.width=240U,.height=320U};
    else j=(DG_Job){.operation=DG_FILL,.dst=&dg_scratch_a,.width=240U,.height=320U,.color=(uint16_t)(n*0x1357U)};
    if(DG_Start(&j,1U)!=DG_OK) DG_DiagnosticStop();
    uint32_t overlap=0U;
    for(uint32_t i=0U;i<2048U;i++) {
      uint32_t active_before=DMA2D->CR&DMA2D_CR_START;
      uint16_t e=(uint16_t)(i*0x9E37U^(i>>7)^n);
      cpu[i]=e;__DSB();uint16_t a=cpu[i];
      uint32_t active_after=DMA2D->CR&DMA2D_CR_START;
      if(active_before&&active_after) {g_dma2d_diagnostic.overlap_cpu_accesses+=2U;overlap=1U;}
      if(a!=e) mismatch(LCD_SDRAM_TEST_ADDRESS+i*2U,e,a);
    }
    if(overlap) g_dma2d_diagnostic.overlap_batches++;
    if(DG_Wait(50U)!=DG_OK) DG_DiagnosticStop();
    uint16_t e=(uint16_t)((n&1U)?(n-1U)*0x1357U:n*0x1357U);
    volatile uint16_t *dst=(volatile uint16_t *)(uintptr_t)j.dst->address;
    for(uint32_t i=0U;i<76800U;i++) {
      uint16_t a=dst[i];if(a!=e) mismatch(j.dst->address+i*2U,e,a);
    }
    /* Separate second read catches retention/addressing errors in CPU region. */
    for(uint32_t i=0U;i<2048U;i++) {
      uint16_t e2=(uint16_t)(i*0x9E37U^(i>>7)^n),a=cpu[i];
      if(a!=e2) mismatch(LCD_SDRAM_TEST_ADDRESS+i*2U,e2,a);
    }
    if(g_dma2d_diagnostic.state==LCD_STATE_FAIL||g_lcd_diagnostic.state==LCD_STATE_FAIL) DG_DiagnosticStop();
    g_dma2d_diagnostic.stress_transfers++;g_dma2d_diagnostic.stress_pixels+=76800U;
    if((n&63U)==0U) verify_framebuffer();
    g_dma2d_diagnostic.stress_elapsed_tick_ms=HAL_GetTick()-g_dma2d_diagnostic.stress_start_tick_ms;
  } while(g_dma2d_diagnostic.stress_elapsed_tick_ms<63000U);
  /* +5% HAL ticks gives >60 s for previously measured +2.4% HSI error. */
  verify_framebuffer();snapshot_configuration(0U);
  if(!g_dma2d_diagnostic.overlap_batches||g_dma2d_diagnostic.state==LCD_STATE_FAIL) {DG_Fail(DG_VERIFY);DG_DiagnosticStop();}
  g_dma2d_diagnostic.stage_pass_mask|=1U<<11;DG_Checkpoint();
}
void DG_TestRun(void)
{
  if(DG_Init()!=DG_OK) {DG_DiagnosticStop();}
  guards_init();snapshot_configuration(1U);DG_Checkpoint();
  fill_tests();copy_pfc_blend_tests();irq_tests();
  /* Static image throughout benchmark/stress; previous destructive full test is excluded. */
  display_image(7U);benchmarks();concurrent_load();
  DG_Registers r;DG_Snapshot(&r);g_dma2d_diagnostic.after=r;
  const uint32_t primask=__get_PRIMASK();__disable_irq();
  if(g_dma2d_diagnostic.state!=LCD_STATE_FAIL&&g_lcd_diagnostic.state!=LCD_STATE_FAIL)
    g_dma2d_diagnostic.state=LCD_STATE_PASS;
  __set_PRIMASK(primask);
  display_image(0U);DG_Checkpoint();
}
void DG_TestProcess(void)
{
  if(g_dma2d_diagnostic.state==LCD_STATE_FAIL||g_lcd_diagnostic.state==LCD_STATE_FAIL) DG_DiagnosticStop();
  uint32_t now=HAL_GetTick(),phase=g_dma2d_diagnostic.display_phase;
  if(phase==9U&&now-display_last_animation_tick_ms>=50U) {
    screen_rect(display_x_pixels,152U,16U,16U,0U);
    display_x_pixels=1U+(display_x_pixels-1U+4U)%223U;
    screen_rect(display_x_pixels,152U,16U,16U,0xF81FU);
    g_dma2d_diagnostic.animation_updates++;display_last_animation_tick_ms=now;
  }
  if(now-display_start_tick_ms>=(phase<5U?2000U:phase==9U?10000U:4000U)) display_image((phase+1U)%10U);
}
