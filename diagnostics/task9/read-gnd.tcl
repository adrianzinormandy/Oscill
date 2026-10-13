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
set start [ms]
set done 0
set state 1
while {!$done && $state != 3 && [ms]-$start < 80000} {
    set done [lindex [read_memory 0x20004228 32 1] 0]
    set state [lindex [read_memory 0x200036A4 32 1] 0]
    if {!$done && $state != 3} {sleep 500}
}
if {!$done && $state != 3} {echo NOT_COMPLETE_NO_HALT; shutdown}
halt
wait_halt 3000
echo FAST_G2_COMPLETE_$done
echo ADC_STATE_$state
echo [reg pc force]
dump_image {J:/MX projects/Oscill/diagnostics/task9/diag-GND.bin} 0x200036A0 2784
dump_image {J:/MX projects/Oscill/diagnostics/task9/scope-GND.bin} 0x20004228 240
dump_image {J:/MX projects/Oscill/diagnostics/task9/adc-GND.bin} 0x20002EA0 2048
foreach a {0x40012000 0x40012004 0x40012008 0x40012010 0x40012304 0x40000000 0x40000004 0x40000028 0x4000002C 0x40026410 0x40026414 0x40026418 0x4002641C 0x40026424 0x40026400 0x40023808 0x4002388C 0xA0000140 0xA0000144 0xA0000148 0xA000014C 0xA0000154 0xA0000158 0x40016818 0x40016884 0x40016894 0x400168AC 0xE000ED28 0xE000ED2C} {echo [mdw $a 1]}
resume
shutdown

