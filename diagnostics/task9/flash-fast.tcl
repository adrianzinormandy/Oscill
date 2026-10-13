# One ST-LINK client. Disable IDE target hooks that rewrite clocks/debug registers.
source [find interface/stlink-dap.cfg]
transport select dapdirect_swd
source [find target/stm32f4x.cfg]
stm32f4x.cpu configure -event examine-end {}
stm32f4x.cpu configure -event reset-init {}
stm32f4x.cpu configure -event gdb-attach {}
adapter speed 1000
reset_config srst_only srst_nogate connect_assert_srst
gdb_port disabled
tcl_port disabled
telnet_port disabled
init
reset halt
set id [lindex [read_memory 0xE0042000 32 1] 0]
if {($id & 0xFFF) != 0x419} {error "Unexpected MCU ID"}
program {J:/MX projects/Oscill/diagnostics/task9/oscill-fast.elf} verify
reset run
shutdown
