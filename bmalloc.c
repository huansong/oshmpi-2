#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "shmem-internals.h"

typedef struct ptr_sizes_s
{
	void * ptr;
	size_t size;
	struct ptr_sizes_s * next;
} ptr_size;

ptr_size * shmallocd_ptrs_sizes;

/* this should be a macro or static-inline for performance */
MPI_Aint translate_remote_address_to_sheap_disp(void * address)
{
	MPI_Aint disp;
#ifdef USE_SMP_OPTIMIZATIONS
	disp = (MPI_Aint)((char *)address - (char *)shmem_smp_sheap_ptrs[shmem_world_rank]);
#else
	disp = (MPI_Aint)((char *)address - (char *)shmem_sheap_base_ptrs[shmem_world_rank]);
#endif	
	/* verify remote access is within symm_heap 
	 * no-debug mode should disable this check */
	if (disp<0 || disp>shmem_sheap_size)
		__shmem_abort(shmem_sheap_size, "Access out of sheap size limit");

	return disp;
}

/* dispense mem from sheap in getpagesize() chunks */
void * bmem_alloc (size_t size)
{
	void * ptr = (void *)shmem_sheap_current_ptr;
	ptr_size * curr = (ptr_size *)malloc (sizeof(ptr_size));
	/* we might need a macro, as this particular signature
	   may not be available in all unix-like systems */	
	long pg_sz = sysconf(_SC_PAGESIZE);
	if ((size % pg_sz) == 0)	
		curr->size = size;
	else { 
		size_t align_bump = (size%pg_sz ? 1 : 0);
		size_t align_size = (size/pg_sz + align_bump) * pg_sz;
		curr->size = align_size; 
	}
	/* Increment current pointer by passed size and check
	   whether it overflows */
	shmem_sheap_current_ptr += curr->size;
	MPI_Aint current_size = translate_remote_address_to_sheap_disp(shmem_sheap_current_ptr);

	if ((size_t)current_size >= shmem_sheap_size) {
		printf ("[E] Insufficient memory in sheap\n");
		shmem_sheap_current_ptr -= curr->size;
		return NULL;
	}
	
	curr->ptr = ptr;

	/* Current head */
	if (shmallocd_ptrs_sizes == NULL) {
		shmallocd_ptrs_sizes = curr;
		shmallocd_ptrs_sizes->next = NULL;
	}
	else {
		curr->next = shmallocd_ptrs_sizes;
		shmallocd_ptrs_sizes = curr;
	}

#ifdef END_SHEAP_ROUTINES_WITH_BARRIER
	shmem_barrier_all();
#endif
	return ptr;
}

void bmem_free (void * ptr)
{
	ptr_size * curr, * prev;
	curr = shmallocd_ptrs_sizes;
	prev = NULL;

	if (ptr == NULL) {
		printf ("[W] Invalid pointer to free\n");
		return;
	}
	
	/* search for ptr in the linked-list */
	while((curr->ptr != ptr) && (curr->next != NULL))
	{
		prev = curr;
		curr = curr->next;
	}
	
	/* deduct the size */
	shmem_sheap_current_ptr -= curr->size;

	/* ptr should be found */	
	if (curr->ptr == ptr) {
		if (prev == NULL) /* node is the first node which is at head */
			shmallocd_ptrs_sizes = curr->next;
		else /* node is somewhere between head and tail */
			prev->next = curr->next;		
		
		free (curr);
	}
#ifdef END_SHEAP_ROUTINES_WITH_BARRIER
	shmem_barrier_all();
#endif	
	return;
}

void * bmem_realloc (void * ptr, size_t size)
{
	/* Find passed pointer info */
	ptr_size * curr;
	curr = shmallocd_ptrs_sizes;
	
	while (curr) {
		if (curr->ptr == ptr)
			break;
		curr = curr->next;
	}
	/* Behaves like shmalloc */
	if (curr == NULL) {
		ptr = bmem_alloc (size);
		return ptr;
	}
	/* No need to decrement shmem_sheap_current_ptr, as bmem_free will
	   take care of it */
	void * new_ptr = bmem_alloc (size);
	memcpy (new_ptr, ptr, curr->size);
	bmem_free (ptr); /* free old pointer */

#ifdef END_SHEAP_ROUTINES_WITH_BARRIER
	shmem_barrier_all();
#endif
	return new_ptr;	
}

/*
   The logic is taken from stackoverflow post:
http://stackoverflow.com/questions/227897/solve-the-memory-alignment-in-c-interview-question-that-stumped-me
 */
void * bmem_align (size_t alignment, size_t size)
{
	/* Notes: Sayan: This will flip the bits */
	uintptr_t mask = ~(uintptr_t)(alignment - 1);
	void * ptr = shmem_sheap_current_ptr;
	
	shmem_sheap_current_ptr += (size + alignment - 1);
	MPI_Aint current_size = translate_remote_address_to_sheap_disp(shmem_sheap_current_ptr);

	if ((size_t)current_size >= shmem_sheap_size) {
		printf ("[E] Address not within symm heap range: returning NULL\n");
		return NULL;
	}
	
	/* Notes: Sayan: Add alignment to the first pointer, suppose it
	returns a bad alignment, then fix it by and-ing with mask, eg: 1+0 = 0 */
	void * mem = (void *)(((uintptr_t)ptr + alignment - 1) & mask);
		
	/* book-keeping */
	ptr_size * curr = (ptr_size *)malloc (sizeof(ptr_size));
	curr->size = size;
	curr->ptr = ptr;

	/* Current head */
	if (shmallocd_ptrs_sizes == NULL) {
		shmallocd_ptrs_sizes = curr;
		shmallocd_ptrs_sizes->next = NULL;
	}
	else {
		curr->next = shmallocd_ptrs_sizes;
		shmallocd_ptrs_sizes = curr;
	}
	
#ifdef END_SHEAP_ROUTINES_WITH_BARRIER
	shmem_barrier_all();
#endif
	return mem;
}
