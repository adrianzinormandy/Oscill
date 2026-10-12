init
poll
set start [ms]
set next 15000
set state 1
while {$state==1&&[ms]-$start<180000} {
 set state [read_memory 0x2000083c 32 1]
 if {[ms]-$start>=$next} {echo POWER_CYCLE_PROGRESS_MS_[expr {[ms]-$start}];echo [mdw 0x20000838 9];set next [expr {$next+15000}]}
 if {$state==1} {sleep 1000}
}
halt
wait_halt 3000
echo POWER_CYCLE_FINAL_STATE_$state
echo [reg pc force]
echo [mdw 0x20000838 80]
dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-dma2d-task7/diag-POWER_CYCLE_FINAL.bin 0x20000838 5544
dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-dma2d-task7/lcd-POWER_CYCLE_FINAL.bin 0x20000640 416
foreach a {0xA0000140 0xA0000144 0xA0000148 0xA000014C 0xA0000154 0xA0000158 0x40016818 0x40016834 0x40016838 0x40016884 0x40016894 0x400168AC 0x400168B0 0x400168B4 0x4002B000 0x4002B004 0xE000ED28 0xE000ED2C} {echo [mdw $a 1]}
if {$state==2} {resume}
shutdown
