# rtl.f -- Verilator source list for bio_bdma_wrapper (+define+SIM build)
#
# Order matters for ONE reason: template_v0.1.sv defines the `thereg /
# `theregfull text macros that bio_bdma.sv and amba_components.sv use
# UNconditionally. Verilator carries `define state forward across files in
# this list, and `resetall (used in ram_1rw_s.sv) does NOT clear macros, so
# listing the template first makes the macros visible everywhere downstream.
#
# include search paths (for any `include directives)
-Irtl
-Irtl/lib

rtl/lib/template.sv
rtl/lib/apb_sfr.sv

# packages
rtl/lib/axi_pkg.sv
rtl/lib/daric_cfg_pkg.sv
rtl/lib/gnrl_sramc_pkg.sv

# interfaces
rtl/lib/amba_interface_def.sv
rtl/lib/axi_intf.sv
rtl/lib/io_interface_def.sv

# amba glue + crossbar + cdc
rtl/lib/amba_components.sv
rtl/lib/axil_reg_if.v
rtl/lib/axil_reg_if_rd.v
rtl/lib/axil_reg_if_wr.v
rtl/lib/axil_register_rd.v
rtl/lib/axil_register_wr.v
rtl/lib/axil_crossbar.v
rtl/lib/axil_crossbar_addr.v
rtl/lib/axil_crossbar_rd.v
rtl/lib/axil_crossbar_wr.v
rtl/lib/axil_cdc.v
rtl/lib/axil_cdc_rd.v
rtl/lib/axil_cdc_wr.v
rtl/lib/cdc_blinded.v
rtl/lib/cdc_level_to_pulse.sv
rtl/lib/arbiter.v
rtl/lib/priority_encoder.v

# cells + memory (behavioral models active under +define+SIM)
rtl/lib/icg.v
rtl/lib/bioram1kx32.v
rtl/ram_1rw_s.sv

# core blocks
rtl/regfifo.v
rtl/pio_divider.v
rtl/picorv32.v
rtl/bio_bdma.sv
rtl/bio_bdma_wrapper.sv

# axi2ahb bridge
rtl/axi2ahb/axi2ahb.v
rtl/axi2ahb/axi2ahb_cmd.v
rtl/axi2ahb/axi2ahb_ctrl.v
rtl/axi2ahb/axi2ahb_rd_fifo.v
rtl/axi2ahb/axi2ahb_wr_fifo.v
rtl/axi2ahb/prgen_fifo.v