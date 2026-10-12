from pathlib import Path
import struct,re,json,sys
p=Path(__file__).parent;t=(p/'layout-DIGITAL.txt').read_text(encoding='utf-8-sig');b=(p/'diag-DC-INPUT.bin').read_bytes()
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
(p/'dc-input-results.json').write_text(json.dumps(x,indent=2),encoding='utf-8')
for v in x['rates']+x['static_load']:
 print(v['rate_target_hz'],v['rate_estimate_hz'],v['elapsed_tick_ms'],v['ht'],v['tc'],v['processed'],v['samples_received'],v['mean'],v['rms_ac'],v['min'],v['max'])
