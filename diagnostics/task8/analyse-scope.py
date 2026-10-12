from pathlib import Path
import struct,re,json,sys
p=Path(__file__).parent;t=(p/'layout-DIGITAL.txt').read_text(encoding='utf-8-sig');b=(p/'diag-SCOPE.bin').read_bytes()
blocks=t.split('type = struct {')[1:]
layouts=[[(int(o),ty,name) for o,n,ty,name in re.findall(r'/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*(uint32_t|uint64_t|float)\s+(\w+);',z)] for z in blocks]
def read(offset,layout):
 return {name:struct.unpack_from({'uint32_t':'<I','uint64_t':'<Q','float':'<f'}[ty],b,offset+o)[0] for o,ty,name in layout}
x=read(0,layouts[0]);fields={name:int(o) for o,n,ty,name in re.findall(r'/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*(AC_Registers|AC_TestResult|SDRAM_RegisterSnapshot|LCD_RegisterSnapshot)\s+(\w+)',blocks[0])}
stride=int(re.search(r'total size \(bytes\):\s*(\d+)',blocks[1])[1])
for name in ('initial','current','first_error_snapshot'):x[name]=read(fields[name],layouts[2])
for name in ('rates','static_load','graphics_load'):
 x[name]=[]
 for i in range(4 if name=='rates' else 1):
  start=fields[name]+i*stride;v=read(start,layouts[1]);v['before']=read(start+144,layouts[2]);v['after']=read(start+240,layouts[2]);x[name].append(v)
for name,count in [('fmc_before',6),('fmc_after',6),('ltdc_before',20),('ltdc_after',20)]:x[name]=list(struct.unpack_from('<'+'I'*count,b,fields[name]))
(p/'scope-results.json').write_text(json.dumps(x,indent=2),encoding='utf-8')
for v in x['graphics_load']:
 print(v['rate_target_hz'],v['rate_estimate_hz'],v['elapsed_tick_ms'],v['ht'],v['tc'],v['processed'],v['samples_received'],v['mean'],v['rms_ac'],v['min'],v['max'])

sb=(p/'scope-SCOPE.bin').read_bytes()
raw_layout=(p/'layout-scope.txt').read_bytes()
st=raw_layout.decode('utf-16' if raw_layout.startswith(b'\xff\xfe') else 'utf-8-sig')
sl=[(int(o),ty,name) for o,n,ty,name in re.findall(r'/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*(uint32_t|uint64_t)\s+(\w+);',st)]
scope={name:struct.unpack_from({'uint32_t':'<I','uint64_t':'<Q'}[ty],sb,o)[0] for o,ty,name in sl}
v=x['graphics_load'][0]
# Prior operator Fs from PG13; unchanged RCC and ARR. This is an estimate
# unless the marker is measured again during the graphics firmware.
hclk_hz=99799.04*1626
elapsed_cycles=v['elapsed_tick_ms']*160000
scope['time_conversion']='estimate from previous operator PG13=48.73 Hz; unchanged RCC and ARR'
scope['estimated_hclk_hz']=hclk_hz
scope['estimated_duration_s']=elapsed_cycles/hclk_hz
scope['estimated_fps']=scope['g2_frames']/scope['estimated_duration_s']
scope['capture_work_percent']=scope['g2_capture_work_cycles']/elapsed_cycles*100
scope['graphics_work_percent']=scope['g2_graphics_work_cycles']/elapsed_cycles*100
scope['work_percent_note']='AC_Process + SV_Process only, nested IRQ included; not total CPU utilisation'
for field in ('g2_render_max_cycles','g2_capture_call_max_cycles','g2_graphics_call_max_cycles'):
 scope[field.replace('_cycles','_estimated_us')]=scope[field]*1e6/hclk_hz
(p/'scope-view-results.json').write_text(json.dumps(scope,indent=2),encoding='utf-8')
print(json.dumps(scope,indent=2))
