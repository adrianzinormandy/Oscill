source [find interface/stlink-dap.cfg]
transport select dapdirect_swd
source [find target/stm32f4x.cfg]
stm32f4x.cpu configure -event examine-end {}
stm32f4x.cpu configure -event reset-init {}
stm32f4x.cpu configure -event gdb-attach {}
adapter speed 1000
reset_config srst_only srst_nogate connect_deassert_srst
gdb_port disabled
tcl_port disabled
telnet_port disabled
init
poll
if {![info exists capture_tag]} {error "Missing capture_tag"}
set requested [ms]
set mode [lindex [read_memory 0x200042C0 32 1] 0]
while {$mode != 1 && [ms]-$requested < 80000} {
    sleep 500
    set mode [lindex [read_memory 0x200042C0 32 1] 0]
}
if {$mode != 1} {error "Trigger RISE was not observed; capture not accepted"}
set first_blocks [lindex [read_memory 0x200036E0 32 1] 0]
set first_matches [lindex [read_memory 0x200042CC 32 1] 0]
set first_frames [lindex [read_memory 0x2000422C 32 1] 0]
set first_button [lindex [read_memory 0x200042DC 32 1] 0]
echo TRIGGER_ON_START_BLOCKS_$first_blocks
echo TRIGGER_ON_START_MATCHES_$first_matches
echo TRIGGER_ON_START_FRAMES_$first_frames
echo TRIGGER_ON_START_BUTTON_$first_button
set began [ms]
set state 1
while {[ms]-$began < 65000 && $state != 3 && $mode == 1} {
    sleep 500
    set state [lindex [read_memory 0x200036A4 32 1] 0]
    set mode [lindex [read_memory 0x200042C0 32 1] 0]
}
set elapsed [expr {[ms]-$began}]
echo TRIGGER_ON_OBSERVED_HOST_MS_$elapsed
echo TRIGGER_ON_END_MODE_$mode
halt
wait_halt 3000
echo LIVE_CAPTURE_$capture_tag
echo ADC_STATE_$state
echo [reg pc force]
dump_image "J:/MX projects/Oscill/diagnostics/task9/diag-$capture_tag.bin" 0x200036A0 2784
dump_image "J:/MX projects/Oscill/diagnostics/task9/scope-$capture_tag.bin" 0x20004228 240
dump_image "J:/MX projects/Oscill/diagnostics/task9/adc-$capture_tag.bin" 0x20002EA0 2048
dump_image "J:/MX projects/Oscill/diagnostics/task9/dma2d-$capture_tag.bin" 0x20000888 5544
dump_image "J:/MX projects/Oscill/diagnostics/task9/lcd-$capture_tag.bin" 0x20000690 416
foreach a {0xE0042008 0x40016824 0x40012000 0x40012004 0x40012008 0x40012010 0x40012304 0x40000000 0x40000004 0x40000028 0x4000002C 0x40026410 0x40026414 0x40026418 0x4002641C 0x40026424 0x40026400 0x40023808 0x4002388C 0xA0000140 0xA0000144 0xA0000148 0xA000014C 0xA0000154 0xA0000158 0x40016818 0x40016884 0x40016894 0x400168AC 0xE000ED28 0xE000ED2C} {echo [mdw $a 1]}
resume
shutdown
