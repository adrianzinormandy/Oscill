from pathlib import Path
import struct, re, json, sys
p=Path(__file__).parent
tag=sys.argv[1] if len(sys.argv)>1 else "GND"
blocks=(p/"layout-fast.txt").read_text(encoding="utf-8-sig").split("type = struct {")[1:]
fmt={"uint32_t":"<I","uint64_t":"<Q","float":"<f"}
def read(data, body, base=0):
    return {name:struct.unpack_from(fmt[ty],data,base+int(offset))[0]
        for offset,ty,name in re.findall(r"/\*\s*(\d+)\s*\|\s*\d+\s*\*/\s*(uint32_t|uint64_t|float)\s+(\w+);",body)}
b=(p/f"diag-{tag}.bin").read_bytes()
a=read(b,blocks[1]); s=read((p/f"scope-{tag}.bin").read_bytes(),blocks[0])
nested={name:int(offset) for offset,ty,name in re.findall(r"/\*\s*(\d+)\s*\|\s*\d+\s*\*/\s*(AC_Registers|AC_TestResult)\s+(\w+)",blocks[1])}
for name in ("initial","current","first_error_snapshot"):
    a[name]=read(b,blocks[3],nested[name])
for name in ("static_load","graphics_load"):
    a[name]=read(b,blocks[2],nested[name])
    a[name]["before"]=read(b,blocks[3],nested[name]+144)
    a[name]["after"]=read(b,blocks[3],nested[name]+240)
gb=(p/"layout-graphics.txt").read_text(encoding="utf-8-sig").split("type = struct {")[1:]
graphics={}
for name,body in zip(("dma2d","lcd"),gb):
    path=p/f"{name}-{tag}.bin"
    if path.exists(): graphics[name]=read(path.read_bytes(),body)
raw=struct.unpack("<1024H",(p/f"adc-{tag}.bin").read_bytes())
mean=sum(raw)/len(raw); variance=sum(v*v for v in raw)/len(raw)-mean*mean
result={"adc":a,"scope":s,**graphics,"snapshot":{"min":min(raw),"max":max(raw),"mean":mean,"rms_ac":max(0,variance)**.5}}
(p/f"results-{tag}.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
print(json.dumps({k:a[k] for k in ("state","last_error","blocks_completed","blocks_processed","stale_block_count","queue_overflow_count","max_processing_cycles","max_latency_cycles","first_error_snapshot")},indent=2))
print(json.dumps(s,indent=2))