/*
 * This file is part of ltrace.
 * Copyright (C) 2012 Edgar E. Iglesias, Axis Communications
 * Copyright (C) 2010 Arnaud Patard, Mandriva SA
 * Copyright (C) 2008,2009 Juan Cespedes
 * Copyright (C) 2006 Eric Vaitl, Cisco Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <asm/ptrace.h>
#include <assert.h>

#include "backend.h"
#include "common.h"
#include "debug.h"
#include "mips.h"
#include "proc.h"
#include "type.h"

#if (!defined(PTRACE_PEEKUSER) && defined(PTRACE_PEEKUSR))
# define PTRACE_PEEKUSER PTRACE_PEEKUSR
#endif

#if (!defined(PTRACE_POKEUSER) && defined(PTRACE_POKEUSR))
# define PTRACE_POKEUSER PTRACE_POKEUSR
#endif


/**
   \addtogroup mips Mips specific functions.

   These are the functions that it looks like I need to implement in
   order to get ltrace to work on our target.

   @{
 */

/**
   \param proc The process that had an event.

   Called by \c next_event() right after the return from wait.

   Most targets just return here. A couple use proc->arch_ptr for a
   private data area.
 */
void
get_arch_dep(Process *proc) {
}

/**
   \param proc Process that had event.
   \param status From \c wait()
   \param sysnum 0-based syscall number.
   \return 1 if syscall, 2 if sysret, 0 otherwise.

   Called by \c next_event() after the call to get_arch_dep()

   It seems that the ptrace call trips twice on a system call, once
   just before the system call and once when it returns. Both times,
   the pc points at the instruction just after the mips "syscall"
   instruction.

   There are several possiblities for system call sets, each is offset
   by a base from the others. On our system, it looks like the base
   for the system calls is 4000.
 */
int
syscall_p(Process *proc, int status, int *sysnum) {
	if (WIFSTOPPED(status)
			&& WSTOPSIG(status) == (SIGTRAP | proc->tracesysgood)) {
		/* get the user's pc (plus 8) */
		long pc = (long)get_instruction_pointer(proc);
		/* fetch the SWI instruction */
		int insn = ptrace(PTRACE_PEEKTEXT, proc->pid, pc - 4, 0);
		int num = ptrace(PTRACE_PEEKTEXT, proc->pid, pc - 8, 0);

		/*
		   On a mips,  syscall looks like:
		   24040fa1    li v0, 0x0fa1   # 4001 --> _exit syscall
		   0000000c    syscall
		 */
		if(insn!=0x0000000c){
			return 0;
		}

		*sysnum = (num & 0xFFFF) - 4000;
		/* if it is a syscall, return 1 or 2 */
		if (proc->callstack_depth > 0 &&
				proc->callstack[proc->callstack_depth - 1].is_syscall &&
				proc->callstack[proc->callstack_depth - 1].c_un.syscall == *sysnum) {
			return 2;
		}

		if (*sysnum >= 0) {
			return 1;
		}
	}
	return 0;
}

/* Based on GDB code.  */
#define mips32_op(x) (x >> 26)
#define itype_op(x) (x >> 26)
#define itype_rs(x) ((x >> 21) & 0x1f)
#define itype_rt(x) ((x >> 16) & 0x1f)
#define itype_immediate(x) (x & 0xffff)

#define jtype_op(x) (x >> 26)
#define jtype_target(x) (x & 0x03ffffff)

#define rtype_op(x) (x >> 26)
#define rtype_rs(x) ((x >> 21) & 0x1f)
#define rtype_rt(x) ((x >> 16) & 0x1f)
#define rtype_rd(x) ((x >> 11) & 0x1f)
#define rtype_shamt(x) ((x >> 6) & 0x1f)
#define rtype_funct(x) (x & 0x3f)

static int32_t
mips32_relative_offset (uint32_t inst)
{
  return ((itype_immediate(inst) ^ 0x8000) - 0x8000) << 2;
}

int mips_next_pcs(struct Process *proc, uint32_t pc, uint32_t *newpc)
{
	uint32_t inst, rx;
	int op;
	int rn;
	int nr = 0;

	inst = ptrace(PTRACE_PEEKTEXT, proc->pid, pc, 0);

	if ((inst & 0xe0000000) != 0) {
		/* Check for branches.  */
		if (itype_op(inst) >> 2 == 5) {
			/* BEQL, BNEL, BLEZL, BGTZL: bits 0101xx */
			op = (itype_op(inst) & 0x03);
			switch (op)
			{
			case 0:	/* BEQL */
			case 1: /* BNEL */
			case 2:	/* BLEZL */
			case 3:	/* BGTZL */
				newpc[nr++] = pc + 8;
				newpc[nr++] = pc + 4 +
					mips32_relative_offset(inst);
				break;
			default:
				newpc[nr++] = pc + 4;
				break;
			}
		} else if (itype_op(inst) == 17 && itype_rs(inst) == 8) {
			/* Step over the branch.  */
			newpc[nr++] = pc + 8;
			newpc[nr++] = pc + mips32_relative_offset(inst) + 4;
		} else {
			newpc[nr++] = pc + 4;
		}
	} else {
		/* Further subdivide into SPECIAL, REGIMM and other.  */
		switch (op = itype_op(inst) & 0x07)
		{
		case 0:
			op = rtype_funct(inst);
			switch (op)
			{
			case 8:	/* JR  */
			case 9:	/* JALR  */
				rn = rtype_rs(inst);

				rx = ptrace(PTRACE_PEEKUSER,proc->pid, rn, 0);
				newpc[nr++] = rx;
				break;
			default:
			case 12:	/* SYSCALL  */
				newpc[nr++] = pc + 4;
				break;
			}
			break;
		case 1:
			op = itype_rt(inst);
			switch (op)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 16:
				case 17:
				case 18:
				case 19:
					newpc[nr++] = pc + 8;
					newpc[nr++] = pc + 4 +
						mips32_relative_offset(inst);
					break;
				default:
					newpc[nr++] = pc + 4;
					break;
			}
			break;
		case 2:	/* J  */
		case 3:	/* JAL  */
			rx = jtype_target(inst) << 2;
			/* Upper four bits get never changed...  */
			newpc[nr++] = rx + ((pc + 4) & ~0x0fffffff);
			break;
		case 4: /* BEQ  */
			if (itype_rs(inst) == itype_rt(inst)) {
				/* Compare the same reg for equality, always
				 * follow the branch.  */
				newpc[nr++] = pc + 4 +
					mips32_relative_offset(inst);
				break;
			}
			/* Fall through.  */
		default:
		case 5:
		case 6:
		case 7:
			/* Step over the branch.  */
			newpc[nr++] = pc + 8;
			newpc[nr++] = pc + mips32_relative_offset(inst) + 4;
			break;
		}
	}
	if (nr <= 0 || nr > 2)
		goto fail;
	if (nr == 2) {
		if (newpc[1] == 0)
			goto fail;
	}
	if (newpc[0] == 0)
		goto fail;

	assert(nr == 1 || nr == 2);
	return nr;

fail:
	printf("nr=%d pc=%x\n", nr, pc);
	printf("pc=%x %x\n", newpc[0], newpc[1]);
	return 0;
}

int
arch_atomic_singlestep(struct Process *proc, struct breakpoint *sbp,
		       int (*add_cb)(void *addr, void *data),
		       void *add_cb_data)
{
	uint32_t pc = (uint32_t) get_instruction_pointer(proc);
	uint32_t newpcs[2];
	int nr;

	nr = mips_next_pcs(proc, pc, newpcs);

	while (nr-- > 0) {
		arch_addr_t baddr = (arch_addr_t) newpcs[nr];
		/* Not sure what to do here. We've already got a bp?  */
		if (dict_find_entry(proc->leader->breakpoints, baddr) != NULL) {
			fprintf(stderr, "skip %p %p\n", baddr, add_cb_data);
			continue;
		}

		if (add_cb(baddr, add_cb_data) < 0)
			return -1;
	}

	ptrace(PTRACE_SYSCALL, proc->pid, 0, 0);
	return 0;
}

/**
   \param type Function/syscall call or return.
   \param proc The process that had an event.
   \param arg_num -1 for return value,
   \return The argument to fetch.

   A couple of assumptions.

-  Type is LT_TOF_FUNCTIONR or LT_TOF_SYSCALLR if arg_num==-1. These
   types are only used in calls for output_right(), which only uses -1
   for arg_num.
-  Type is LT_TOF_FUNCTION or LT_TOF_SYSCALL for args 0...4.
-   I'm only displaying the first 4 args (Registers a0..a3). Good
   enough for now.

  Mips conventions seem to be:
- syscall parameters: r4...r9
- syscall return: if(!a3){ return v0;} else{ errno=v0;return -1;}
- function call: r4..r7. Not sure how to get arg number 5.
- function return: v0

The argument registers are wiped by a call, so it is a mistake to ask
for arguments on a return. If ltrace does this, we will need to cache
arguments somewhere on the call.

I'm not doing any floating point support here.

*/
long
gimme_arg(enum tof type, Process *proc, int arg_num, struct arg_type_info *info)
{
	long ret;
	long addr;
	debug(2,"type %d arg %d",type,arg_num);
	if (arg_num == -1) {
		if(type == LT_TOF_FUNCTIONR) {
			return  ptrace(PTRACE_PEEKUSER,proc->pid,off_v0,0);
		}
		if (type == LT_TOF_SYSCALLR) {
			unsigned a3=ptrace(PTRACE_PEEKUSER, proc->pid,off_a3,0);
			unsigned v0=ptrace(PTRACE_PEEKUSER, proc->pid,off_v0,0);
			if(!a3){
				return v0;
			}
			return -1;
		}
	}
	if (type == LT_TOF_FUNCTION || type == LT_TOF_SYSCALL) {
		/* o32: float args are in f12 and f14 */
		if ((info->type == ARGTYPE_FLOAT) && (arg_num < 2)) {
			ret=ptrace(PTRACE_PEEKUSER,proc->pid,off_fpr0+12+arg_num*2,0);
			debug(2,"ret = %#lx",ret);
			return ret;
		}
		if(arg_num <4){
			ret=ptrace(PTRACE_PEEKUSER,proc->pid,off_a0+arg_num,0);
			debug(2,"ret = %#lx",ret);
			return ret;
		} else {
			/* not sure it's going to work for something else than syscall */
			addr=ptrace(PTRACE_PEEKUSER,proc->pid,off_sp,0);
			if (addr == -1) {
				debug(2,"ret = %#lx",addr);
				return addr;
			}
			ret = addr + 4*arg_num;
			ret=ptrace(PTRACE_PEEKTEXT,proc->pid,addr,0);
			debug(2,"ret = %#lx",ret);
			return ret;
		}
	}
	if (type == LT_TOF_FUNCTIONR || type == LT_TOF_SYSCALLR){
		addr=ptrace(PTRACE_PEEKUSER,proc->pid,off_sp,0);
		if (addr == -1) {
			debug(2,"ret = %#lx",addr);
			return addr;
		}
		ret = addr + 4*arg_num;
		ret=ptrace(PTRACE_PEEKTEXT,proc->pid,addr,0);
		debug(2,"ret = %#lx",ret);
		return ret;
	}
	fprintf(stderr, "gimme_arg called with wrong arguments\n");
	return 0;
}

/**@}*/
