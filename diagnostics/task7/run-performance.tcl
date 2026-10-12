proc snapshot {label} {
 echo SNAPSHOT_$label
 echo [reg pc force]
 dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-dma2d-task7/diag-$label.bin 0x20000838 5544
 dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-dma2d-task7/lcd-$label.bin 0x20000640 416
 foreach a {0x40023800 0x40023804 0x40023808 0x40023830 0x40023838 0x40023844 0x40023888 0x4002388C 0x4002B000 0x4002B004 0x4002B00C 0x4002B010 0x4002B014 0x4002B018 0x4002B01C 0x4002B024 0x4002B034 0x4002B038 0x4002B03C 0x4002B040 0x4002B044 0xA0000140 0xA0000144 0xA0000148 0xA000014C 0xA0000154 0xA0000158 0x40016818 0x40016834 0x40016838 0x40016884 0x40016894 0x400168AC 0x400168B0 0x400168B4 0x400168B8 0xE000ED28 0xE000ED2C 0xE0001000 0xE0001004} {echo [mdw $a 1]}
}
init
reset halt
bp 0x08005a30 2 hw
bp 0x08005a28 2 hw
bp 0x08001c60 2 hw
set failed 0
set host_start [ms]
set stress_start 0
for {set i 0} {$i<13} {incr i} {
 resume
 set checkpoint_start [ms]
 set next_progress 10000
 while {[stm32f4x.cpu curstate]!="halted"&&[ms]-$checkpoint_start<150000} {
  sleep 1000
  set elapsed [expr {[ms]-$checkpoint_start}]
  if {$elapsed>=$next_progress} {echo PROGRESS_HOST_MS_$elapsed;echo [mdw 0x20000838 9];set next_progress [expr {$next_progress+10000}]}
 }
 if {[stm32f4x.cpu curstate]!="halted"} {halt;wait_halt 3000;set failed 1;break}
 echo [reg pc force]
 echo [mdw 0x20000838 9]
 set state [read_memory 0x2000083c 32 1]
 set stage [read_memory 0x20000838 32 1]
 if {$state==3} {set failed 1;break}
 if {$stage==1} {snapshot INITIAL}
 if {$stage==9} {dump_image C:/Users/Tony_/AppData/Local/Temp/oscill-dma2d-task7/benchmarks.bin 0x20000838 5544}
 if {$stage==10} {set stress_start [ms]}
 if {$stage==11} {echo STRESS_HOST_MS_[expr {[ms]-$stress_start}]}
 if {$stage==8&&$state==2} {break}
}
snapshot FINAL
echo [mdw 0x20000838 80]
echo TOTAL_HOST_MS_[expr {[ms]-$host_start}]
rbp 0x08005a30
rbp 0x08005a28
rbp 0x08001c60
if {!$failed} {resume}
echo OPERATOR_DISPLAY_RUNNING_FAILURE_$failed
shutdown
