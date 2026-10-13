# Read-only localization of the edge direction; TIM2 freezes on core halt.
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
halt
wait_halt 3000
echo [reg pc force]
echo RENDER_PHASE_[lindex [read_memory 0x200047F8 32 1] 0]
# LTDC remains active while the core is halted; allow an outstanding VBR to finish.
sleep 25
set front [lindex [read_memory 0x400168AC 32 1] 0]
echo ACTUAL_FRAMEBUFFER_$front
if {$front != 0xD0000000 && $front != 0xD0040000} {error "Unexpected frontbuffer"}
dump_image {J:/MX projects/Oscill/diagnostics/task9/frame-edge.bin} $front 153600
dump_image {J:/MX projects/Oscill/diagnostics/task9/columns-min-edge.bin} 0x20004318 608
dump_image {J:/MX projects/Oscill/diagnostics/task9/columns-max-edge.bin} 0x20004578 608
dump_image {J:/MX projects/Oscill/diagnostics/task9/scope-EDGE.bin} 0x20004228 240
foreach a {0x40016824 0x400168AC 0xE000ED28 0xE000ED2C} {echo [mdw $a 1]}
resume
shutdown
