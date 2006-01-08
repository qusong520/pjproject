/* $Id$ */
/* 
 * Copyright (C)2003-2006 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include <pj/timer.h>
#include <pj/pool.h>
#include <pj/os.h>
#include <pj/string.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/lock.h>

#define HEAP_PARENT(X)	(X == 0 ? 0 : (((X) - 1) / 2))
#define HEAP_LEFT(X)	(((X)+(X))+1)


#define DEFAULT_MAX_TIMED_OUT_PER_POLL  (64)


/**
 * The implementation of timer heap.
 */
struct pj_timer_heap_t
{
    /** Pool from which the timer heap resize will get the storage from */
    pj_pool_t *pool;

    /** Maximum size of the heap. */
    pj_size_t max_size;

    /** Current size of the heap. */
    pj_size_t cur_size;

    /** Max timed out entries to process per poll. */
    unsigned max_entries_per_poll;

    /** Lock object. */
    pj_lock_t *lock;

    /** Autodelete lock. */
    pj_bool_t auto_delete_lock;

    /**
     * Current contents of the Heap, which is organized as a "heap" of
     * pj_timer_entry *'s.  In this context, a heap is a "partially
     * ordered, almost complete" binary tree, which is stored in an
     * array.
     */
    pj_timer_entry **heap;

    /**
     * An array of "pointers" that allows each pj_timer_entry in the
     * <heap_> to be located in O(1) time.  Basically, <timer_id_[i]>
     * contains the slot in the <heap_> array where an pj_timer_entry
     * with timer id <i> resides.  Thus, the timer id passed back from
     * <schedule_entry> is really an slot into the <timer_ids> array.  The
     * <timer_ids_> array serves two purposes: negative values are
     * treated as "pointers" for the <freelist_>, whereas positive
     * values are treated as "pointers" into the <heap_> array.
     */
    pj_timer_id_t *timer_ids;

    /**
     * "Pointer" to the first element in the freelist contained within
     * the <timer_ids_> array, which is organized as a stack.
     */
    pj_timer_id_t timer_ids_freelist;

    /** Callback to be called when a timer expires. */
    pj_timer_heap_callback *callback;

};



PJ_INLINE(void) lock_timer_heap( pj_timer_heap_t *ht )
{
    if (ht->lock) {
	pj_lock_acquire(ht->lock);
    }
}

PJ_INLINE(void) unlock_timer_heap( pj_timer_heap_t *ht )
{
    if (ht->lock) {
	pj_lock_release(ht->lock);
    }
}


static void copy_node( pj_timer_heap_t *ht, int slot, pj_timer_entry *moved_node )
{
    PJ_CHECK_STACK();

    // Insert <moved_node> into its new location in the heap.
    ht->heap[slot] = moved_node;
    
    // Update the corresponding slot in the parallel <timer_ids_> array.
    ht->timer_ids[moved_node->_timer_id] = slot;
}

static pj_timer_id_t pop_freelist( pj_timer_heap_t *ht )
{
    // We need to truncate this to <int> for backwards compatibility.
    pj_timer_id_t new_id = ht->timer_ids_freelist;
    
    PJ_CHECK_STACK();

    // The freelist values in the <timer_ids_> are negative, so we need
    // to negate them to get the next freelist "pointer."
    ht->timer_ids_freelist =
	-ht->timer_ids[ht->timer_ids_freelist];
    
    return new_id;
    
}

static void push_freelist (pj_timer_heap_t *ht, pj_timer_id_t old_id)
{
    PJ_CHECK_STACK();

    // The freelist values in the <timer_ids_> are negative, so we need
    // to negate them to get the next freelist "pointer."
    ht->timer_ids[old_id] = -ht->timer_ids_freelist;
    ht->timer_ids_freelist = old_id;
}


static void reheap_down(pj_timer_heap_t *ht, pj_timer_entry *moved_node,
                        size_t slot, size_t child)
{
    PJ_CHECK_STACK();

    // Restore the heap property after a deletion.
    
    while (child < ht->cur_size)
    {
	// Choose the smaller of the two children.
	if (child + 1 < ht->cur_size
	    && PJ_TIME_VAL_LT(ht->heap[child + 1]->_timer_value, ht->heap[child]->_timer_value))
	    child++;
	
	// Perform a <copy> if the child has a larger timeout value than
	// the <moved_node>.
	if (PJ_TIME_VAL_LT(ht->heap[child]->_timer_value, moved_node->_timer_value))
        {
	    copy_node( ht, slot, ht->heap[child]);
	    slot = child;
	    child = HEAP_LEFT(child);
        }
	else
	    // We've found our location in the heap.
	    break;
    }
    
    copy_node( ht, slot, moved_node);
}

static void reheap_up( pj_timer_heap_t *ht, pj_timer_entry *moved_node,
		       size_t slot, size_t parent)
{
    // Restore the heap property after an insertion.
    
    while (slot > 0)
    {
	// If the parent node is greater than the <moved_node> we need
	// to copy it down.
	if (PJ_TIME_VAL_LT(moved_node->_timer_value, ht->heap[parent]->_timer_value))
        {
	    copy_node(ht, slot, ht->heap[parent]);
	    slot = parent;
	    parent = HEAP_PARENT(slot);
        }
	else
	    break;
    }
    
    // Insert the new node into its proper resting place in the heap and
    // update the corresponding slot in the parallel <timer_ids> array.
    copy_node(ht, slot, moved_node);
}


static pj_timer_entry * remove_node( pj_timer_heap_t *ht, size_t slot)
{
    pj_timer_entry *removed_node = ht->heap[slot];
    
    // Return this timer id to the freelist.
    push_freelist( ht, removed_node->_timer_id );
    
    // Decrement the size of the heap by one since we're removing the
    // "slot"th node.
    ht->cur_size--;
    
    // Set the ID
    removed_node->_timer_id = -1;

    // Only try to reheapify if we're not deleting the last entry.
    
    if (slot < ht->cur_size)
    {
	int parent;
	pj_timer_entry *moved_node = ht->heap[ht->cur_size];
	
	// Move the end node to the location being removed and update
	// the corresponding slot in the parallel <timer_ids> array.
	copy_node( ht, slot, moved_node);
	
	// If the <moved_node->time_value_> is great than or equal its
	// parent it needs be moved down the heap.
	parent = HEAP_PARENT (slot);
	
	if (PJ_TIME_VAL_GTE(moved_node->_timer_value, ht->heap[parent]->_timer_value))
	    reheap_down( ht, moved_node, slot, HEAP_LEFT(slot));
	else
	    reheap_up( ht, moved_node, slot, parent);
    }
    
    return removed_node;
}

static void grow_heap(pj_timer_heap_t *ht)
{
    // All the containers will double in size from max_size_
    size_t new_size = ht->max_size * 2;
    pj_timer_id_t *new_timer_ids;
    pj_size_t i;
    
    // First grow the heap itself.
    
    pj_timer_entry **new_heap = 0;
    
    new_heap = pj_pool_alloc(ht->pool, sizeof(pj_timer_entry*) * new_size);
    memcpy(new_heap, ht->heap, ht->max_size * sizeof(pj_timer_entry*));
    //delete [] this->heap_;
    ht->heap = new_heap;
    
    // Grow the array of timer ids.
    
    new_timer_ids = 0;
    new_timer_ids = pj_pool_alloc(ht->pool, new_size * sizeof(pj_timer_id_t));
    
    memcpy( new_timer_ids, ht->timer_ids, ht->max_size * sizeof(pj_timer_id_t));
    
    //delete [] timer_ids_;
    ht->timer_ids = new_timer_ids;
    
    // And add the new elements to the end of the "freelist".
    for (i = ht->max_size; i < new_size; i++)
	ht->timer_ids[i] = -((pj_timer_id_t) (i + 1));
    
    ht->max_size = new_size;
}

static void insert_node(pj_timer_heap_t *ht, pj_timer_entry *new_node)
{
    if (ht->cur_size + 2 >= ht->max_size)
	grow_heap(ht);
    
    reheap_up( ht, new_node, ht->cur_size, HEAP_PARENT(ht->cur_size));
    ht->cur_size++;
}


static pj_status_t schedule_entry( pj_timer_heap_t *ht,
				   pj_timer_entry *entry, 
				   const pj_time_val *future_time )
{
    if (ht->cur_size < ht->max_size)
    {
	// Obtain the next unique sequence number.
	// Set the entry
	entry->_timer_id = pop_freelist(ht);
	entry->_timer_value = *future_time;
	insert_node( ht, entry);
	return 0;
    }
    else
	return -1;
}


static int cancel( pj_timer_heap_t *ht, 
		   pj_timer_entry *entry, 
		   int dont_call)
{
  long timer_node_slot;

  PJ_CHECK_STACK();

  // Check to see if the timer_id is out of range
  if (entry->_timer_id < 0 || (pj_size_t)entry->_timer_id > ht->max_size)
    return 0;

  timer_node_slot = ht->timer_ids[entry->_timer_id];

  if (timer_node_slot < 0) // Check to see if timer_id is still valid.
    return 0;

  if (entry != ht->heap[timer_node_slot])
    {
      pj_assert(entry == ht->heap[timer_node_slot]);
      return 0;
    }
  else
    {
      remove_node( ht, timer_node_slot);

      if (dont_call == 0)
        // Call the close hook.
	(*ht->callback)(ht, entry);
      return 1;
    }
}


/*
 * Calculate memory size required to create a timer heap.
 */
PJ_DEF(pj_size_t) pj_timer_heap_mem_size(pj_size_t count)
{
    return /* size of the timer heap itself: */
           sizeof(pj_timer_heap_t) + 
           /* size of each entry: */
           (count+2) * (sizeof(pj_timer_entry*)+sizeof(pj_timer_id_t)) +
           /* lock, pool etc: */
           132;
}

/*
 * Create a new timer heap.
 */
PJ_DEF(pj_status_t) pj_timer_heap_create( pj_pool_t *pool,
					  pj_size_t size,
                                          pj_timer_heap_t **p_heap)
{
    pj_timer_heap_t *ht;
    pj_size_t i;

    PJ_ASSERT_RETURN(pool && p_heap, PJ_EINVAL);

    *p_heap = NULL;

    /* Magic? */
    size += 2;

    /* Allocate timer heap data structure from the pool */
    ht = pj_pool_alloc(pool, sizeof(pj_timer_heap_t));
    if (!ht)
        return PJ_ENOMEM;

    /* Initialize timer heap sizes */
    ht->max_size = size;
    ht->cur_size = 0;
    ht->max_entries_per_poll = DEFAULT_MAX_TIMED_OUT_PER_POLL;
    ht->timer_ids_freelist = 1;
    ht->pool = pool;

    /* Lock. */
    ht->lock = NULL;
    ht->auto_delete_lock = 0;

    // Create the heap array.
    ht->heap = pj_pool_alloc(pool, sizeof(pj_timer_entry*) * size);
    if (!ht->heap)
        return PJ_ENOMEM;

    // Create the parallel
    ht->timer_ids = pj_pool_alloc( pool, sizeof(pj_timer_id_t) * size);
    if (!ht->timer_ids)
        return PJ_ENOMEM;

    // Initialize the "freelist," which uses negative values to
    // distinguish freelist elements from "pointers" into the <heap_>
    // array.
    for (i=0; i<size; ++i)
	ht->timer_ids[i] = -((pj_timer_id_t) (i + 1));

    *p_heap = ht;
    return PJ_SUCCESS;
}

PJ_DEF(void) pj_timer_heap_destroy( pj_timer_heap_t *ht )
{
    if (ht->lock && ht->auto_delete_lock) {
        pj_lock_destroy(ht->lock);
        ht->lock = NULL;
    }
}

PJ_DEF(void) pj_timer_heap_set_lock(  pj_timer_heap_t *ht,
                                      pj_lock_t *lock,
                                      pj_bool_t auto_del )
{
    if (ht->lock && ht->auto_delete_lock)
        pj_lock_destroy(ht->lock);

    ht->lock = lock;
    ht->auto_delete_lock = auto_del;
}


PJ_DEF(unsigned) pj_timer_heap_set_max_timed_out_per_poll(pj_timer_heap_t *ht,
                                                          unsigned count )
{
    unsigned old_count = ht->max_entries_per_poll;
    ht->max_entries_per_poll = count;
    return old_count;
}

PJ_DEF(pj_timer_entry*) pj_timer_entry_init( pj_timer_entry *entry,
                                             int id,
                                             void *user_data,
                                             pj_timer_heap_callback *cb )
{
    pj_assert(entry && cb);

    entry->id = id;
    entry->user_data = user_data;
    entry->cb = cb;

    return entry;
}

PJ_DEF(pj_status_t) pj_timer_heap_schedule( pj_timer_heap_t *ht,
					    pj_timer_entry *entry, 
					    const pj_time_val *delay)
{
    pj_status_t status;
    pj_time_val expires;

    PJ_ASSERT_RETURN(ht && entry && delay, PJ_EINVAL);
    PJ_ASSERT_RETURN(entry->cb != NULL, PJ_EINVAL);

    pj_gettimeofday(&expires);
    PJ_TIME_VAL_ADD(expires, *delay);
    
    lock_timer_heap(ht);
    status = schedule_entry(ht, entry, &expires);
    unlock_timer_heap(ht);

    return status;
}

PJ_DEF(int) pj_timer_heap_cancel( pj_timer_heap_t *ht,
				  pj_timer_entry *entry)
{
    int count;

    PJ_ASSERT_RETURN(ht && entry, PJ_EINVAL);

    lock_timer_heap(ht);
    count = cancel(ht, entry, 1);
    unlock_timer_heap(ht);

    return count;
}

PJ_DEF(unsigned) pj_timer_heap_poll( pj_timer_heap_t *ht, 
                                     pj_time_val *next_delay )
{
    pj_time_val now;
    unsigned count;

    PJ_ASSERT_RETURN(ht, 0);

    if (!ht->cur_size && next_delay) {
	next_delay->sec = next_delay->msec = PJ_MAXINT32;
	return 0;
    }

    count = 0;
    pj_gettimeofday(&now);

    lock_timer_heap(ht);
    while ( ht->cur_size && 
	    PJ_TIME_VAL_LTE(ht->heap[0]->_timer_value, now) &&
            count < ht->max_entries_per_poll ) 
    {
	pj_timer_entry *node = remove_node(ht, 0);
	++count;

	unlock_timer_heap(ht);
	if (node->cb)
	    (*node->cb)(ht, node);
	lock_timer_heap(ht);
    }
    if (ht->cur_size && next_delay) {
	*next_delay = ht->heap[0]->_timer_value;
	PJ_TIME_VAL_SUB(*next_delay, now);
    } else if (next_delay) {
	next_delay->sec = next_delay->msec = PJ_MAXINT32;
    }
    unlock_timer_heap(ht);

    return count;
}

PJ_DEF(pj_size_t) pj_timer_heap_count( pj_timer_heap_t *ht )
{
    PJ_ASSERT_RETURN(ht, 0);

    return ht->cur_size;
}

PJ_DEF(pj_status_t) pj_timer_heap_earliest_time( pj_timer_heap_t * ht,
					         pj_time_val *timeval)
{
    pj_assert(ht->cur_size != 0);
    if (ht->cur_size == 0)
        return PJ_ENOTFOUND;

    lock_timer_heap(ht);
    *timeval = ht->heap[0]->_timer_value;
    unlock_timer_heap(ht);

    return PJ_SUCCESS;
}

