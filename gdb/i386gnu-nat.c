/* Low level interface to i386 running the GNU Hurd.
   Copyright (C) 1992, 1995, 1996 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include "inferior.h"
#include "floatformat.h"

#include <assert.h>
#include <stdio.h>
#include <errno.h>

#include <mach.h>
#include <mach_error.h>
#include <mach/message.h>
#include <mach/exception.h>

#include "gnu-nat.h"

/* The FPU hardware state.  */
struct env387
{
  unsigned short control;
  unsigned short r0;
  unsigned short status;
  unsigned short r1;
  unsigned short tag;
  unsigned short r2;
  unsigned long eip;
  unsigned short code_seg;
  unsigned short opcode;
  unsigned long operand;
  unsigned short operand_seg;
  unsigned short r3;
  unsigned char regs[8][10];
};


/* Offset to the thread_state_t location where REG is stored.  */
#define REG_OFFSET(reg) offsetof (struct i386_thread_state, reg)

/* At reg_offset[i] is the offset to the thread_state_t location where
   the gdb registers[i] is stored.  */
static int reg_offset[] =
{
  REG_OFFSET (eax), REG_OFFSET (ecx), REG_OFFSET (edx), REG_OFFSET (ebx),
  REG_OFFSET (uesp), REG_OFFSET (ebp), REG_OFFSET (esi), REG_OFFSET (edi),
  REG_OFFSET (eip), REG_OFFSET (efl), REG_OFFSET (cs), REG_OFFSET (ss),
  REG_OFFSET (ds), REG_OFFSET (es), REG_OFFSET (fs), REG_OFFSET (gs)
};

#define REG_ADDR(state, regnum) ((char *)(state) + reg_offset[regnum])


/* Get the whole floating-point state of THREAD and record the
   values of the corresponding (pseudo) registers.  */
static void
fetch_fpregs (struct proc *thread)
{
  mach_msg_type_number_t count = i386_FLOAT_STATE_COUNT;
  struct i386_float_state state;
  struct env387 *ep = (struct env387 *) state.hw_state;
  error_t err;
  int i;

  err = thread_get_state (thread->port, i386_FLOAT_STATE,
			  (thread_state_t) &state, &count);
  if (err)
    {
      warning ("Couldn't fetch floating-point state from %s",
	       proc_string (thread));
      return;
    }

  if (! state.initialized)
    /* The floating-point state isn't initialized.  */
    {
      for (i = FP0_REGNUM; i <= FP7_REGNUM; i++)
	supply_register (i, NULL);
      for (i = FIRST_FPU_CTRL_REGNUM; i <= LAST_FPU_CTRL_REGNUM; i++)
	supply_register (i, NULL);

      return;
    }

  /* Supply the floating-point registers.  */
  for (i = 0; i < 8; i++)
    supply_register (FP0_REGNUM + i, ep->regs[i]);

  supply_register (FCTRL_REGNUM, (char *) &ep->control);
  supply_register (FSTAT_REGNUM, (char *) &ep->status);
  supply_register (FTAG_REGNUM,  (char *) &ep->tag);
  supply_register (FCOFF_REGNUM, (char *) &ep->eip);
  supply_register (FDS_REGNUM,   (char *) &ep->operand_seg);
  supply_register (FDOFF_REGNUM, (char *) &ep->operand);

  /* Store the code segment and opcode pseudo registers.  */
  {
    long l;

    l = ep->code_seg;
    supply_register (FCS_REGNUM, (char *) &l);
    l = ep->opcode & ((1 << 11) - 1);
    supply_register (FOP_REGNUM, (char *) &l);
  }
}

/* Fetch register REGNO, or all regs if REGNO is -1.  */
void
gnu_fetch_registers (int regno)
{
  struct proc *thread;

  /* Make sure we know about new threads.  */
  inf_update_procs (current_inferior);

  thread = inf_tid_to_thread (current_inferior, inferior_pid);
  if (!thread)
    error ("Can't fetch registers from thread %d: No such thread",
	   inferior_pid);

  if (regno < NUM_GREGS || regno == -1)
    {
      thread_state_t state;
      
      /* This does the dirty work for us.  */
      state = proc_get_state (thread, 0);
      if (!state)
	{
	  warning ("Couldn't fetch registers from %s",
		   proc_string (thread));
	  return;
	}

      if (regno == -1)
	{
	  int i;
	  
	  proc_debug (thread, "fetching all register");
	  
	  for (i = 0; i < NUM_GREGS; i++)
	    supply_register (i, REG_ADDR (state, i));
	  thread->fetched_regs = ~0;
	}
      else
	{
	  proc_debug (thread, "fetching register %s", REGISTER_NAME (regno));
	  
	  supply_register (regno, REG_ADDR (state, regno));
	  thread->fetched_regs |= (1 << regno);
	}
    }

  if (regno >= NUM_GREGS || regno == -1)
    {
      proc_debug (thread, "fetching floating-point registers");
      
      fetch_fpregs (thread);
    }
}


/* Fill the i387 hardware state EP with selected data from the set of
   (pseudo) registers specified by REGS and VALID.  VALID is an array
   indicating which registers in REGS are valid.  If VALID is zero,
   all registers are assumed to be valid.  */
static void
convert_to_env387 (struct env387 *ep, char *regs, signed char *valid)
{
  int i;

  /* Fill in the floating-point registers.  */
  for (i = 0; i < 8; i++)
    if (!valid || valid[i])
      memcpy (ep->regs[i], &regs[REGISTER_BYTE (FP0_REGNUM + i)],
	      REGISTER_RAW_SIZE (FP0_REGNUM + i));

#define fill(member, regno)                                              \
  if (!valid || valid[(regno)])                                          \
    memcpy (&ep->member, &regs[REGISTER_BYTE (regno)],                   \
            sizeof (ep->member));

  fill (control, FCTRL_REGNUM);
  fill (status, FSTAT_REGNUM);
  fill (tag, FTAG_REGNUM);
  fill (eip, FCOFF_REGNUM);
  fill (operand, FDOFF_REGNUM);
  fill (operand_seg, FDS_REGNUM);

#undef fill

  if (!valid || valid[FCS_REGNUM])
    ep->code_seg =
      (* (int *) &registers[REGISTER_BYTE (FCS_REGNUM)] & 0xffff);
  
  if (!valid || valid[FOP_REGNUM])
    ep->opcode =
      ((* (int *) &registers[REGISTER_BYTE (FOP_REGNUM)] & ((1 << 11) - 1)));
}

/* Store the whole floating-point state into THREAD using information
   from the corresponding (pseudo) registers.  */
static void
store_fpregs (struct proc *thread)
{
  mach_msg_type_number_t count = i386_FLOAT_STATE_COUNT;
  struct i386_float_state state;
  error_t err;

  err = thread_get_state (thread->port, i386_FLOAT_STATE,
			  (thread_state_t) &state, &count);
  if (err)
    {
      warning ("Couldn't fetch floating-point state from %s",
	       proc_string (thread));
      return;
    }

  convert_to_env387 ((struct env387 *) state.hw_state,
		     registers, register_valid);
    
  err = thread_set_state (thread->port, i386_FLOAT_STATE,
			  (thread_state_t) &state, i386_FLOAT_STATE_COUNT);
  if (err)
    {
      warning ("Couldn't store floating-point state into %s",
	       proc_string (thread));
      return;
    }
}

/* Store at least register REGNO, or all regs if REGNO == -1.  */
void
gnu_store_registers (int regno)
{
  struct proc *thread;

  /* Make sure we know about new threads.  */
  inf_update_procs (current_inferior);

  thread = inf_tid_to_thread (current_inferior, inferior_pid);
  if (!thread)
    error ("Couldn't store registers into thread %d: No such thread",
	   inferior_pid);

  if (regno < NUM_GREGS || regno == -1)
    {
      thread_state_t state;
      thread_state_data_t old_state;
      int was_aborted = thread->aborted;
      int was_valid = thread->state_valid;

      if (!was_aborted && was_valid)
	memcpy (&old_state, &thread->state, sizeof (old_state));

      state = proc_get_state (thread, 1);
      if (!state)
	{
	  warning ("Couldn't store registers into %s", proc_string (thread));
	  return;
	}

      if (!was_aborted && was_valid)
	/* See which registers have changed after aborting the thread.  */
	{
	  int check_regno;

	  for (check_regno = 0; check_regno < NUM_GREGS; check_regno++)
	    if ((thread->fetched_regs & (1 << check_regno))
		&& memcpy (REG_ADDR (&old_state, check_regno),
			   REG_ADDR (state, check_regno),
			   REGISTER_RAW_SIZE (check_regno)))
	      /* Register CHECK_REGNO has changed!  Ack!  */
	      {
		warning ("Register %s changed after the thread was aborted",
			 REGISTER_NAME (check_regno));
		if (regno >= 0 && regno != check_regno)
		  /* Update gdb's copy of the register.  */
		  supply_register (check_regno, REG_ADDR (state, check_regno));
		else
		  warning ("... also writing this register!  Suspicious...");
	      }
	}

#define fill(state, regno)                                               \
  memcpy (REG_ADDR(state, regno), &registers[REGISTER_BYTE (regno)],     \
          REGISTER_RAW_SIZE (regno))

      if (regno == -1)
	{
	  int i;
	  
	  proc_debug (thread, "storing all registers");

	  for (i = 0; i < NUM_GREGS; i++)
	    if (register_valid[i])
	      fill (state, i);
	}
      else
	{
	  proc_debug (thread, "storing register %s", REGISTER_NAME (regno));

	  assert (register_valid[regno]);
	  fill (state, regno);
	}
    }

#undef fill

  if (regno >= NUM_GREGS || regno == -1)
    {
      proc_debug (thread, "storing floating-point registers");
      
      store_fpregs (thread);
    }
}
