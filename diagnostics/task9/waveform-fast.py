from pathlib import Path
import struct,json,sys
p=Path(__file__).parent
tag=sys.argv[1]
fmin=float(sys.argv[2]) if len(sys.argv)>2 else 242.7*2048
fmax=float(sys.argv[3]) if len(sys.argv)>3 else 243.2*2048
s=struct.unpack("<1024H",(p/f"adc-{tag}.bin").read_bytes())
# Mid-code crossings, same physical sample clock. No harmonic fit/calibration.
rising=[i-1+(2048-a)/(b-a) for i,(a,b) in enumerate(zip(s,s[1:]),1) if a<2048<=b]
falling=[i-1+(a-2048)/(a-b) for i,(a,b) in enumerate(zip(s,s[1:]),1) if a>=2048>b]
periods=[b-a for a,b in zip(rising,rising[1:])]
period=sum(periods)/len(periods) if periods else None
result={"tag":tag,"fs_measured_sps":[fmin,fmax],"samples":len(s),"min_code":min(s),"max_code":max(s),"rising_edges":len(rising),"falling_edges":len(falling),"period_samples_mean":period,"period_samples_min":min(periods) if periods else None,"period_samples_max":max(periods) if periods else None,"frequency_from_measured_fs_hz":[fmin/period,fmax/period] if period else None,"display_window_intervals":100,"cycles_per_display_window":100/period if period else None}
(p/f"waveform-{tag}.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
print(json.dumps(result,indent=2))
