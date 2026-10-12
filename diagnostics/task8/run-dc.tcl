init
poll
set start [ms]
set next 2000
set state 1
while {$state==1&&[ms]-$start<30000} {
 set state [read_memory 0x20003684 32 1]
 if {[ms]-$start>=$next} {echo ADC_PROGRESS_MS_[expr {[ms]-$start}];echo [mdw 0x20003680 4];set next [expr {$next+2000}]}
 if {$state==1} {sleep 250}
}
halt
wait_halt 3000
echo ADC_FINAL_STATE_$state
echo [reg pc force]
echo [mdw 0x20003680 48]
dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-adc-task8/diag-DC.bin 0x20003680 2784
dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-adc-task8/adc-snapshot-DC.bin 0x20002e80 2048
dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-adc-task8/adc-dma-DC.bin 0x20001e60 4096
foreach a {0x40012000 0x40012004 0x40012008 0x40012010 0x4001202C 0x40012034 0x40012304 0x40000000 0x40000004 0x40000028 0x4000002C 0x40000024 0x40000010 0x40026410 0x40026414 0x40026418 0x4002641C 0x40026424 0x40026400 0x40023808 0x4002388C 0xA0000140 0xA0000144 0xA0000148 0xA000014C 0xA0000154 0xA0000158 0x40016818 0x40016884 0x40016894 0x400168AC 0xE000ED28 0xE000ED2C} {echo [mdw $a 1]}
resume
shutdown
