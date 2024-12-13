#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <current.h>
#include <elf.h>
#include <spl.h>

/* Place your page table functions here */

/* PT init */
int create_pt_l1(paddr_t ** pt) {
    pt = kmalloc(sizeof(paddr_t **) * L1_PT_SIZE);
	if (pt == NULL) return ENOMEM;

	for (int msb = 0; msb < L1_PT_SIZE; msb++) pt[msb] = NULL;
    return 0;
}

int create_pt_l2(paddr_t ** pt, uint32_t msb)
{
    KASSERT(msb < 0xFFFFF800);
    
    if (pt[msb] != NULL) return EINVAL;

    pt[msb] = kmalloc(sizeof(paddr_t) * L2_PT_SIZE);
    if (pt[msb] == NULL) return ENOMEM;

    for (int lsb = 0; lsb < L2_PT_SIZE; lsb++) {
        pt[msb][lsb] = 0;
    }
    
    return 0;
}

int create_pte(paddr_t ** pt, uint32_t msb, uint32_t lsb, uint32_t dirty)
{
    int result = 0;
    if (pt[msb] == NULL) result = create_pt_l2(pt, msb);
    if (result) return result;

    if (pt[msb][lsb] != 0) return EINVAL;

    // allocated a virtual address to page 
    vaddr_t virtual_base = alloc_kpages(1);
    if (virtual_base == 0) return ENOMEM;
    bzero((void *)virtual_base, PAGE_SIZE);

    // converting to physical address
    paddr_t physical_base = KVADDR_TO_PADDR(virtual_base);

    // creating the actual entry in the l2 pt
    // EntryLo: 20bits: PFN | dirty bit | TLBLO_valid
    // TLBLO_valid is a valid entry for the bits in TLB Lo register
    // PAGE_FRAME mask to get the first 20 bits from physical address

    pt[msb][lsb] = (physical_base & PAGE_FRAME) | dirty | TLBLO_VALID;
    return 0;
}

int copy_pt(paddr_t ** pt_original, paddr_t ** pt_copy)
{
    if (pt_original == NULL) return EINVAL;

    if (pt_copy == NULL) pt_copy = kmalloc(sizeof(paddr_t *) * L1_PT_SIZE);

    if (pt_copy == NULL) return ENOMEM;
 
    for (int msb = 0; msb < L1_PT_SIZE; msb++) {
        if (pt_original[msb] != NULL) {
            pt_copy[msb] = kmalloc(sizeof(paddr_t) * L2_PT_SIZE);
            if (pt_copy[msb] == NULL) return ENOMEM;

            for (int lsb = 0; lsb < L2_PT_SIZE; lsb++) {
                pt_copy[msb][lsb] = pt_original[msb][lsb];
                if (pt_copy[msb][lsb] == 0) continue;
                vaddr_t newpage = alloc_kpages(1);
                if (newpage == 0) return ENOMEM;
                bzero((void *)newpage,PAGE_SIZE);
                if (memmove((void *)newpage, (const void *)PADDR_TO_KVADDR(pt_original[msb][lsb] & PAGE_FRAME),
                    PAGE_SIZE) == NULL) { // fail memmove()
                    destroy_pt(pt_copy);
                    return ENOMEM; // Out of memory
                }
                uint32_t dirty = pt_original[msb][lsb] & TLBLO_DIRTY;
                pt_copy[msb][lsb] = (KVADDR_TO_PADDR(newpage) & PAGE_FRAME) | dirty | TLBLO_VALID;
            }   
        }
    }
    return 0;
}

void destroy_pt(paddr_t ** pt)
{
    if (pt == NULL) return;
    for (int msb = 0; msb < L1_PT_SIZE; msb++) {
        if (pt[msb] != NULL) {
            for (int lsb = 0; lsb < L2_PT_SIZE; lsb++) {
                if (pt[msb][lsb] != 0) {
                     free_kpages(PADDR_TO_KVADDR(pt[msb][lsb] & PAGE_FRAME));
                     pt[msb][lsb] = 0;
                }
            }
            kfree(pt[msb]);
        }
    }

    kfree(pt);
}

/* Initialization function */
void vm_bootstrap(void)
{
    /* empty */
}

bool pte_exists(paddr_t ** pt, uint32_t msb, uint32_t lsb) {
    if (pt == NULL) return false;
    if (pt[msb] == NULL) return false;
    if (pt[msb][lsb] == 0) return false;
    return true;
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    switch (faulttype) {
        case VM_FAULT_READONLY:
            return EFAULT;
        case VM_FAULT_READ:
        case VM_FAULT_WRITE:
            break;
        default:
            return EINVAL;
    }
    
    if (faultaddress == 0) return EFAULT;

    if (curproc == NULL) return EFAULT;

    struct addrspace *as = proc_getas();
    if (as == NULL) return EFAULT;

    faultaddress &= PAGE_FRAME;

    // check if it has been allocated a frame
    uint32_t msb = faultaddress >> 21;
    uint32_t lsb = (faultaddress << 11) >> 23;


    if (as->pagetable == NULL) return EFAULT;
    if (as->regions == NULL) return EFAULT;

    uint32_t dirty = 0;
    int result = 0;

    if (!pte_exists(as->pagetable, msb, lsb)) {
        struct region *curr = as->regions;
        for (curr = as->regions; curr != NULL; curr = curr->next) {
            if ((faultaddress < (curr->as_vbase + curr->size)) && faultaddress >= curr->as_vbase) {
                dirty = ((curr->flags & PF_W) == PF_W)? TLBLO_DIRTY : 0;
                break;
            }
        }
        if (curr == NULL) return EFAULT;
        if (((curr->flags & PF_W) != PF_W) && faulttype == VM_FAULT_WRITE) return EFAULT;

        result = create_pte(as->pagetable, msb, lsb, dirty);
        if (result) return result;
    }

    uint32_t entry_hi = faultaddress & PAGE_FRAME;
    uint32_t entry_lo = as->pagetable[msb][lsb];

    int spl = splhigh();
    tlb_random(entry_hi, entry_lo);
    splx(spl);

    return 0;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
    (void)ts;
    panic("vm tried to do tlb shootdown?!\n");
}