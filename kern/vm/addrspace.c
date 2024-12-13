/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <elf.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */
	as->pagetable = NULL;
	as->regions = NULL;
	as->stackbase = USERSTACK;

	as->pagetable = kmalloc(sizeof(paddr_t **) * L1_PT_SIZE);
	if (as->pagetable == NULL) {
		kfree(as);
		return NULL;
	}
	for (int msb = 0; msb < L1_PT_SIZE; msb++) as -> pagetable[msb] = NULL;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas == NULL) {
		return ENOMEM;
	}

	newas->stackbase = old->stackbase;

	/* Copy regions */
	struct region *oldr, *newr, *prev_newr;
	oldr = old->regions;
	newr = NULL;
	prev_newr = NULL;

	while (oldr != NULL) {
		/* allocate memory to new region */
		newr = kmalloc(sizeof(struct region));
		if (newr == NULL) {
			as_destroy(newas);
			return ENOMEM; // out of memory!
		}

		/* copy values */
		newr->as_vbase = oldr->as_vbase;
		// newr->as_pbase = oldr->as_pbase;
		// newr->as_npages = oldr->as_npages;
		newr->size = oldr->size;
		newr->flags = oldr->flags;
		newr->og_flags = oldr->og_flags;
		newr->next = NULL;

		/* LINK the LINKed list */
		if (newas->regions == NULL) {
			newas->regions = newr;
		} else {
			prev_newr->next = newr;
		}

		/* move to next region */
		oldr = oldr->next;
		prev_newr = newr;
		newr = newr->next;
	}
	
	/* Copy page table */
	int res = copy_pt(old->pagetable, newas->pagetable);
	if (res != 0) {
		as_destroy(newas);
		return res; // rip pt ):
	}

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	if (as == NULL) return;
	
	/* Free the regions */
	struct region *current, *next;
	current = as->regions;
	while (current != NULL) {
		next = current->next;
		kfree(current);
		current = next;
	}
	as->regions = NULL;

	/* Free the page table */
	destroy_pt(as->pagetable);
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/*
	 * Write this. Copied from dumbvm.c
	 */
	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
	as_activate();
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	// Aligning the region
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

	struct region *new_region = kmalloc(sizeof(struct region));
	if (new_region == NULL) return ENOMEM;
	uint32_t flags = readable | writeable | executable;
	// if (writeable) flags |= PF_W;
	// if (readable) flags |= PF_R;
	// if (executable) flags |= PF_X;

	new_region->flags = flags;
	new_region->og_flags = flags;
	new_region->as_vbase = vaddr;
	// new_region->as_npages = npages;
	new_region->size = memsize;

	new_region->next = as->regions;
	as->regions = new_region;

	return 0; /* Unimplemented */
}

int
as_prepare_load(struct addrspace *as)
{
	if (as == NULL) return EFAULT;

	struct region *current = as->regions;
	while (current != NULL) {
		current->og_flags = current->flags;
		current->flags = PF_W | PF_R;
		current = current->next;
	}

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	if (as == NULL) return EFAULT;

	struct region *current = as->regions;
	while (current != NULL) {
		current->flags = current->og_flags;
		current = current->next;
	}

	as_deactivate();
	
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	// the stack pointer is the greatest pointer on the stack
	// it is where we extend down
	// to derive the address from the stack pointer, we simply subtract the size
	// of the stack from the pointer
	int result = as_define_region(as, USERSTACK - USERSTACK_SIZE, USERSTACK_SIZE, PF_R, PF_W, PF_X);
	if (result) return result;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}