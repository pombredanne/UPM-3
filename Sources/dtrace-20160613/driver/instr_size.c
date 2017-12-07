/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved	*/


//#pragma ident	"@(#)instr_size.c	1.14	05/07/08 SMI"
#if defined(USERMODE)
typedef unsigned int uint_t;
typedef unsigned long long uint64_t;
typedef unsigned char uchar_t;
typedef unsigned char uint8_t;
# define uintptr_t unsigned long
#include <stdio.h>
#include <string.h>

# define	DATAMODEL_LP64 2
# define	DATAMODEL_NATIVE 2

# define	model_t	int

# else

#include <dtrace_linux.h>
#include <sys/dtrace_impl.h>
#include <dtrace_proto.h>
# if defined(sun)
#include <sys/proc.h>
#include <sys/param.h>
#include <sys/cmn_err.h>
#include <sys/archsystm.h>
#include <sys/copyops.h>
#include <vm/seg_enum.h>
#include <sys/privregs.h>
# endif

#endif

#include "dis_tables.h"

/*
 * This subsystem (with the minor exception of the instr_size() function) is
 * is called from DTrace probe context.  This imposes several requirements on
 * the implementation:
 *
 * 1. External subsystems and functions may not be referenced.  The one current
 *    exception is for cmn_err, but only to signal the detection of table
 *    errors.  Assuming the tables are correct, no combination of input is to
 *    trigger a cmn_err call.
 *
 * 2. These functions can't be allowed to be traced.  To prevent this,
 *    all functions in the probe path (everything except instr_size()) must
 *    have names that begin with "dtrace_".
 */

typedef enum dis_isize {
	DIS_ISIZE_INSTR,
	DIS_ISIZE_OPERAND
} dis_isize_t;


/*
 * get a byte from instruction stream
 */
static int
dtrace_dis_get_byte(void *p)
{
	int ret;
	uchar_t **instr = p;

	ret = **instr;
	*instr += 1;

	return (ret);
}

/*
 * Returns either the size of a given instruction, in bytes, or the size of that
 * instruction's memory access (if any), depending on the value of `which'.
 * If a programming error in the tables is detected, the system will panic to
 * ease diagnosis.  Invalid instructions will not be flagged.  They will appear
 * to have an instruction size between 1 and the actual size, and will be
 * reported as having no memory impact.
 */
/* ARGSUSED2 */
static int
dtrace_dis_isize(uchar_t *instr, dis_isize_t which, model_t model, int *rmindex)
{
# if defined(__arm__)
	if (rmindex != NULL)
		*rmindex = 0;
	return 4;

# elif defined(__amd64) || defined(__i386)
	int sz;
	dis86_t	x;
	uint_t mode = SIZE32;

	mode = (model == DATAMODEL_LP64) ? SIZE64 : SIZE32;

	x.d86_data = (void **)&instr;
	x.d86_get_byte = dtrace_dis_get_byte;
	x.d86_check_func = NULL;

	if (dtrace_disx86(&x, mode) != 0)
		return (-1);

	if (which == DIS_ISIZE_INSTR)
		sz = x.d86_len;		/* length of the instruction */
	else
		sz = x.d86_memsize;	/* length of memory operand */

	if (rmindex != NULL)
		*rmindex = x.d86_rmindex;
	return (sz);
# else
#	error "dtrace_dis_isize: cannot handle this cpu"
#endif

}

int
dtrace_instr_size_isa(uchar_t *instr, model_t model, int *rmindex)
{
	return (dtrace_dis_isize(instr, DIS_ISIZE_INSTR, model, rmindex));
}

int
dtrace_instr_size(uchar_t *instr)
{
	return (dtrace_dis_isize(instr, DIS_ISIZE_INSTR, DATAMODEL_NATIVE,
	    NULL));
}
#if linux
/**********************************************************************/
/*   We  need  the  modrm  byte  of  an instruction for RIP relative  */
/*   addressing for when we single step.			      */
/**********************************************************************/
uchar_t *
dtrace_instr_modrm(uchar_t *instr)
{	int	rmindex = -1;

	dtrace_dis_isize(instr, DIS_ISIZE_INSTR, DATAMODEL_NATIVE,
	    &rmindex);
	return rmindex >= 0 ? instr + rmindex : NULL;
}
/**********************************************************************/
/*   Get instr size and modrm in one hit.			      */
/**********************************************************************/
int
dtrace_instr_size_modrm(uchar_t *instr, int *modrm)
{	int	rmindex = -1;
	int	size;

	size = dtrace_dis_isize(instr, DIS_ISIZE_INSTR, DATAMODEL_NATIVE,
	    &rmindex);
	/***********************************************/
	/*   Handle LOCK prefixes - Sun thinks a lock  */
	/*   prefix  is separate from the instruction  */
	/*   after  it,  but  we  need to single step  */
	/*   these,  and will end up with a LOCK away  */
	/*   from the instruction. Bad news.	       */
	/***********************************************/
if (0)
	switch (*instr) {
	  case 0xf0: // LOCK
	  case 0xf2: // REPZ
	  case 0xf3: // REPNZ
		size += dtrace_dis_isize(instr+1, DIS_ISIZE_INSTR, DATAMODEL_NATIVE,
			&rmindex);
		rmindex++;
	  	break;
	  }
	*modrm = rmindex;
	return size;
}
/**********************************************************************/
/*   Utility  to partially disassemble an instruction. Useful whilst  */
/*   looking around, not needed during core processing.		      */
/**********************************************************************/
void
dtrace_instr_dump(char *label, uint8_t *insn)
{	int	 s = dtrace_instr_size(insn);
	char	buf[128];
	char	*cp;
	int	i;

	snprintf(buf, sizeof buf, "%s %p: ", label, insn);
	cp = buf + strlen(buf);
	for (i = 0; i < s; i++) {
		snprintf(cp, sizeof buf - (cp - buf) - 3, "%02x ", *insn++);
		cp += strlen(cp);
	}
#if !defined(DIS_TEXT)
	*cp++ = '\n';
#endif
	*cp++ = '\0';
	printk("%s", buf);
}
#endif
# if 0
/*ARGSUSED*/
int
instr_size(struct regs *rp, caddr_t *addrp, enum seg_rw rw)
{
	uchar_t instr[16];	/* maximum size instruction */
	caddr_t pc = (caddr_t)rp->r_pc;

	(void) copyin_nowatch(pc, (caddr_t)instr, sizeof (instr));

	return (dtrace_dis_isize(instr,
	    rw == S_EXEC ? DIS_ISIZE_INSTR : DIS_ISIZE_OPERAND,
	    dtrace_data_model(curproc), NULL));
}
# endif