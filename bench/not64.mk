# Not64 CPU core sources, relative to $(NOT64): shared by the bench and the player.
NOT64_SRC	:=	r4300/r4300.c r4300/cop0.c r4300/special.c r4300/regimm.c \
			r4300/exception.c r4300/Invalid_Code.c r4300/Recomp-Cache-Heap.c \
			r4300/interupt.c r4300/cop1.c r4300/tlb.c r4300/cop1_w.c \
			r4300/cop1_s.c r4300/cop1_d.c r4300/recomp.c r4300/bc.c \
			r4300/cop1_l.c r4300/pure_interp.c r4300/ARAM-blocks.c \
			r4300/ppc/MIPS-to-PPC.c r4300/ppc/Recompile.c r4300/ppc/Wrappers.c \
			r4300/ppc/FuncTree.c r4300/ppc/Register-Cache.c \
			gc_memory/tlb.c gc_memory/TLB-Cache-hash.c gc_memory/memory.c \
			gc_memory/dma.c gc_memory/pif.c gc_memory/flashram.c \
			gc_memory/n64_cic_nus_6105.c \
			main/rom_gc.c main/md5.c \
			rsp_hle/alist.c rsp_hle/alist_audio.c rsp_hle/alist_naudio.c rsp_hle/alist_nead.c \
			rsp_hle/audio.c rsp_hle/cicx105.c rsp_hle/hle.c rsp_hle/hvqm.c rsp_hle/jpeg.c \
			rsp_hle/memory.c rsp_hle/mp3.c rsp_hle/musyx.c rsp_hle/plugin.c rsp_hle/re2.c
