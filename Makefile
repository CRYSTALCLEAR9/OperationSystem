# we assume that the utilities from RISC-V cross-compiler are already in PATH.

CROSS_PREFIX := riscv64-unknown-elf-
CC           := $(CROSS_PREFIX)gcc
AR           := $(CROSS_PREFIX)ar
RANLIB       := $(CROSS_PREFIX)ranlib

SRC_DIR      := .
OBJ_DIR      := obj
SPROJS_INCLUDE := -I.

HOSTFS_ROOT := hostfs_root
ifneq (,)
  march := -march=
  is_32bit := $(findstring 32,$(march))
  mabi := -mabi=$(if $(is_32bit),ilp32,lp64)
endif

CFLAGS := -Wall -Werror -fno-builtin -nostdlib -D__NO_INLINE__ -mcmodel=medany -g -gdwarf-2 -Og \
          -std=gnu99 -Wno-unused -Wno-attributes -fno-delete-null-pointer-checks -fno-PIE \
          -fno-omit-frame-pointer -fno-optimize-sibling-calls $(march)
COMPILE := $(CC) -MMD -MP $(CFLAGS) $(SPROJS_INCLUDE)

UTIL_CPPS := $(wildcard util/*.c)
UTIL_OBJS := $(addprefix $(OBJ_DIR)/, $(patsubst %.c,%.o,$(UTIL_CPPS)))
UTIL_LIB  := $(OBJ_DIR)/util.a

KERNEL_LDS  := kernel/kernel.lds
KERNEL_CPPS := $(wildcard kernel/*.c kernel/machine/*.c kernel/util/*.c)
KERNEL_ASMS := $(wildcard kernel/*.S kernel/machine/*.S kernel/util/*.S)
KERNEL_OBJS := $(addprefix $(OBJ_DIR)/, $(patsubst %.c,%.o,$(KERNEL_CPPS)))
KERNEL_OBJS += $(addprefix $(OBJ_DIR)/, $(patsubst %.S,%.o,$(KERNEL_ASMS)))
KERNEL_TARGET := $(OBJ_DIR)/riscv-pke

SPIKE_INF_CPPS := $(wildcard spike_interface/*.c)
SPIKE_INF_OBJS := $(addprefix $(OBJ_DIR)/, $(patsubst %.c,%.o,$(SPIKE_INF_CPPS)))
SPIKE_INF_LIB  := $(OBJ_DIR)/spike_interface.a

USER_APPS := \
	app_ishell \
	app_shell \
	app_ls \
	app_mkdir \
	app_touch \
	app_cat \
	app_echo \
	app_print_backtrace \
	app_errorline \
	app_sum_sequence \
	app_singlepageheap \
	app_wait \
	app_semaphore \
	app_cow \
	app_relativepath \
	app_exec \
	app_shell_bonus \
	app_pipe_echo \
	app_upper \
	app_bgwork \
	app_stress_proc \
	app_stress_io \
	app_stress_suite \
	app_run_app01 \
	app_run_alloc \
	app0 \
	app1 \
	app_alloc0 \
	app_alloc1

USER_OBJS := $(addprefix $(OBJ_DIR)/user/,$(addsuffix .o,$(USER_APPS))) $(OBJ_DIR)/user/user_lib.o
USER_BIN_TARGETS := $(addprefix $(HOSTFS_ROOT)/bin/,$(USER_APPS))
USER_OBJ_TARGETS := $(addprefix $(OBJ_DIR)/,$(USER_APPS))
USER_TARGET := $(HOSTFS_ROOT)/bin/app_shell
HOSTFS_INDEX_STAMP := $(HOSTFS_ROOT)/.dirindex.stamp

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(dir $(UTIL_OBJS))
	@mkdir -p $(dir $(SPIKE_INF_OBJS))
	@mkdir -p $(dir $(KERNEL_OBJS))
	@mkdir -p $(dir $(USER_OBJS))

$(HOSTFS_ROOT)/bin:
	@mkdir -p $@

$(OBJ_DIR)/%.o : %.c
	@echo "compiling" $<
	@$(COMPILE) -c $< -o $@

$(OBJ_DIR)/%.o : %.S
	@echo "compiling" $<
	@$(COMPILE) -c $< -o $@

$(UTIL_LIB): $(OBJ_DIR) $(UTIL_OBJS)
	@echo "linking " $@ ...
	@$(AR) -rcs $@ $(UTIL_OBJS)
	@echo "Util lib has been build into" \"$@\"

$(SPIKE_INF_LIB): $(OBJ_DIR) $(UTIL_OBJS) $(SPIKE_INF_OBJS)
	@echo "linking " $@ ...
	@$(AR) -rcs $@ $(SPIKE_INF_OBJS) $(UTIL_OBJS)
	@echo "Spike lib has been build into" \"$@\"

$(KERNEL_TARGET): $(OBJ_DIR) $(UTIL_LIB) $(SPIKE_INF_LIB) $(KERNEL_OBJS) $(KERNEL_LDS)
	@echo "linking" $@ ...
	@$(COMPILE) $(KERNEL_OBJS) $(UTIL_LIB) $(SPIKE_INF_LIB) -o $@ -T $(KERNEL_LDS)
	@echo "PKE core has been built into" \"$@\"

$(HOSTFS_ROOT)/bin/%: $(OBJ_DIR)/user/%.o $(OBJ_DIR)/user/user_lib.o $(UTIL_LIB) | $(HOSTFS_ROOT)/bin
	@echo "linking" $@ ...
	@$(COMPILE) --entry=main $^ -o $@
	@echo "User app has been built into" \"$@\"

$(OBJ_DIR)/app0: $(OBJ_DIR)/user/app0.o $(OBJ_DIR)/user/user_lib.o $(UTIL_LIB) user/user0.lds
	@echo "linking" $@ ...
	@$(COMPILE) $(OBJ_DIR)/user/app0.o $(OBJ_DIR)/user/user_lib.o $(UTIL_LIB) -o $@ -T user/user0.lds
	@echo "User app has been built into" \"$@\"

$(OBJ_DIR)/app1: $(OBJ_DIR)/user/app1.o $(OBJ_DIR)/user/user_lib.o $(UTIL_LIB) user/user1.lds
	@echo "linking" $@ ...
	@$(COMPILE) $(OBJ_DIR)/user/app1.o $(OBJ_DIR)/user/user_lib.o $(UTIL_LIB) -o $@ -T user/user1.lds
	@echo "User app has been built into" \"$@\"

$(OBJ_DIR)/%: $(OBJ_DIR)/user/%.o $(OBJ_DIR)/user/user_lib.o $(UTIL_LIB)
	@echo "linking" $@ ...
	@$(COMPILE) --entry=main $^ -o $@
	@echo "User app has been built into" \"$@\"

$(HOSTFS_INDEX_STAMP): $(USER_BIN_TARGETS) $(wildcard $(HOSTFS_ROOT)/*) | $(HOSTFS_ROOT)/bin
	@echo "indexing hostfs ..."
	@find $(HOSTFS_ROOT) -type d | while read dir; do \
		find "$$dir" -maxdepth 1 -mindepth 1 ! -name '.dirindex' ! -name '.dirindex.stamp' -printf '%f\n' | LC_ALL=C sort > "$$dir/.dirindex"; \
	done
	@touch $@

-include $(wildcard $(OBJ_DIR)/*/*.d)
-include $(wildcard $(OBJ_DIR)/*/*/*.d)

.DEFAULT_GOAL := all

all: $(KERNEL_TARGET) $(USER_BIN_TARGETS) $(USER_OBJ_TARGETS) $(HOSTFS_INDEX_STAMP)
.PHONY: all

run: $(KERNEL_TARGET) $(USER_TARGET)
	@echo "********************HUST PKE********************"
	spike $(KERNEL_TARGET) /bin/app_shell

gdb: $(KERNEL_TARGET) $(USER_TARGET)
	spike --rbb-port=9824 -H $(KERNEL_TARGET) /bin/app_shell &
	@sleep 1
	openocd -f ./.spike.cfg &
	@sleep 1
	riscv64-unknown-elf-gdb -command=./.gdbinit

gdb_clean:
	@-kill -9 $$(lsof -i:9824 -t)
	@-kill -9 $$(lsof -i:3333 -t)
	@sleep 1

objdump:
	riscv64-unknown-elf-objdump -d $(KERNEL_TARGET) > $(OBJ_DIR)/kernel_dump
	riscv64-unknown-elf-objdump -d $(USER_TARGET) > $(OBJ_DIR)/user_dump

cscope:
	find ./ -name "*.c" > cscope.files
	find ./ -name "*.h" >> cscope.files
	find ./ -name "*.S" >> cscope.files
	find ./ -name "*.lds" >> cscope.files
	cscope -bqk

format:
	@python ./format.py ./

clean:
	rm -fr $(OBJ_DIR) $(HOSTFS_ROOT)/bin
