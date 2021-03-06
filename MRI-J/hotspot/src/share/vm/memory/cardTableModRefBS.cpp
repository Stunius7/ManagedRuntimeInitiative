/*
 * Copyright 2000-2006 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *  
 */
// This file is a derivative work resulting from (and including) modifications
// made by Azul Systems, Inc.  The date of such changes is 2010.
// Copyright 2010 Azul Systems, Inc.  All Rights Reserved.
//
// Please contact Azul Systems, Inc., 1600 Plymouth Street, Mountain View, 
// CA 94043 USA, or visit www.azulsystems.com if you need additional information 
// or have any questions.


// This kind of "BarrierSet" allows a "CollectedHeap" to detect and
// enumerate ref fields that have been modified (since the last
// enumeration.)

#include "cardTableModRefBS.hpp"
#include "cardTableRS.hpp"
#include "java.hpp"
#include "modules.hpp"
#include "mutexLocker.hpp"
#include "os.hpp"
#include "ostream.hpp"
#include "sharedHeap.hpp"
#include "space.hpp"
#include "universe.hpp"
#include "virtualspace.hpp"

#include "allocation.inline.hpp"
#include "oop.inline.hpp"
#include "os_os.inline.hpp"

#include "oop.inline2.hpp"

size_t CardTableModRefBS::cards_required(size_t covered_words)
{
  // Add one for a guard card, used to detect errors.
  const size_t words = align_size_up(covered_words, card_size_in_words);
  return words / card_size_in_words + 1;
}

size_t CardTableModRefBS::compute_byte_map_size()
{
  assert(_guard_index == cards_required(_whole_heap.word_size()) - 1,
                                        "unitialized, check declaration order");
  assert(_page_size != 0, "unitialized, check declaration order");
  const size_t granularity = os::vm_allocation_granularity();
  return align_size_up(_guard_index + 1, MAX2(_page_size, granularity));
}

CardTableModRefBS::CardTableModRefBS(MemRegion whole_heap,
				     int max_covered_regions) :
  ModRefBarrierSet(max_covered_regions),
  _whole_heap(whole_heap),
  _guard_index(cards_required(whole_heap.word_size()) - 1),
  _last_valid_index(_guard_index - 1),
  _page_size(os::vm_page_size()),
  _byte_map_size(compute_byte_map_size())
{
  _kind = BarrierSet::CardTableModRef;

  HeapWord* low_bound  = _whole_heap.start();
  HeapWord* high_bound = _whole_heap.end();
  assert((uintptr_t(low_bound)  & (card_size - 1))  == 0, "heap must start at card boundary");
  assert((uintptr_t(high_bound) & (card_size - 1))  == 0, "heap must end at card boundary");

  assert(card_size <= 512, "card_size must be less than 512"); // why?

  _covered   = new MemRegion[max_covered_regions];
  _committed = new MemRegion[max_covered_regions];
  if (_covered == NULL || _committed == NULL)
    vm_exit_during_initialization("couldn't alloc card table covered region set.");
  int i;
  for (i = 0; i < max_covered_regions; i++) {
    _covered[i].set_word_size(0);
    _committed[i].set_word_size(0);
  }
  _cur_covered_regions = 0;

  const size_t rs_align = _page_size == (size_t) os::vm_page_size() ? 0 :
    MAX2(_page_size, (size_t) os::vm_allocation_granularity());
  ReservedSpace heap_rs(_byte_map_size, (char*) __CARD_TABLE_MOD_REF_BS_START_ADDR__, rs_align);
  if (!heap_rs.is_reserved()) {
vm_exit_during_initialization("Could not reserve enough space for the card marking array");
  }
  // The assember store_check code will do an unsigned shift of the oop, 
  // then add it to byte_map_base, i.e.
  // 
  //   _byte_map = byte_map_base + (uintptr_t(low_bound) >> card_shift)
  _byte_map = (jbyte*) heap_rs.base();
  byte_map_base = _byte_map - (uintptr_t(low_bound) >> card_shift);
  assert(byte_for(low_bound) == &_byte_map[0], "Checking start of map");
  assert(byte_for(high_bound-1) <= &_byte_map[_last_valid_index], "Checking end of map");

  jbyte* guard_card = &_byte_map[_guard_index];
  uintptr_t guard_page = align_size_down((uintptr_t)guard_card, _page_size);
  _guard_region = MemRegion((HeapWord*)guard_page, _page_size);
if(!os::commit_memory((char*)guard_page,_page_size,Modules::CardTableModRefBS)){
    // Do better than this for Merlin
    vm_exit_out_of_memory(_page_size, "card table last card");
  }
  *guard_card = last_card;

   _lowest_non_clean =
    NEW_C_HEAP_ARRAY(CardArr, max_covered_regions);
  _lowest_non_clean_chunk_size =
    NEW_C_HEAP_ARRAY(size_t, max_covered_regions);
  _lowest_non_clean_base_chunk_index =
    NEW_C_HEAP_ARRAY(uintptr_t, max_covered_regions);
  _last_LNC_resizing_collection =
    NEW_C_HEAP_ARRAY(int, max_covered_regions);
  if (_lowest_non_clean == NULL
      || _lowest_non_clean_chunk_size == NULL 
      || _lowest_non_clean_base_chunk_index == NULL 
      || _last_LNC_resizing_collection == NULL)
    vm_exit_during_initialization("couldn't allocate an LNC array.");
  for (i = 0; i < max_covered_regions; i++) {
    _lowest_non_clean[i] = NULL;
    _lowest_non_clean_chunk_size[i] = 0;
    _last_LNC_resizing_collection[i] = -1;
  }

  if (TraceCardTableModRefBS) {
    gclog_or_tty->print_cr("CardTableModRefBS::CardTableModRefBS: ");
    gclog_or_tty->print_cr("  "
"  &_byte_map[0]: "PTR_FORMAT
"  &_byte_map[_last_valid_index]: "PTR_FORMAT,
                  &_byte_map[0],
                  &_byte_map[_last_valid_index]);
    gclog_or_tty->print_cr("  "
"  byte_map_base: "PTR_FORMAT,
                  byte_map_base);
  }
}

int CardTableModRefBS::find_covering_region_by_base(HeapWord* base) {
  int i;
  for (i = 0; i < _cur_covered_regions; i++) {
    if (_covered[i].start() == base) return i;
    if (_covered[i].start() > base) break;
  }
  // If we didn't find it, create a new one.
  assert(_cur_covered_regions < _max_covered_regions,
	 "too many covered regions");
  // Move the ones above up, to maintain sorted order.
  for (int j = _cur_covered_regions; j > i; j--) {
    _covered[j] = _covered[j-1];
    _committed[j] = _committed[j-1];
  }
  int res = i;
  _cur_covered_regions++;
  _covered[res].set_start(base);
  _covered[res].set_word_size(0);
  jbyte* ct_start = byte_for(base);
  uintptr_t ct_start_aligned = align_size_down((uintptr_t)ct_start, _page_size);
  _committed[res].set_start((HeapWord*)ct_start_aligned);
  _committed[res].set_word_size(0);
  return res;
}

int CardTableModRefBS::find_covering_region_containing(HeapWord* addr) {
  for (int i = 0; i < _cur_covered_regions; i++) {
    if (_covered[i].contains(addr)) {
      return i;
    }
  }
  assert(0, "address outside of heap?");
  return -1;
}

HeapWord* CardTableModRefBS::largest_prev_committed_end(int ind) const {
  HeapWord* max_end = NULL;
  for (int j = 0; j < ind; j++) {
    HeapWord* this_end = _committed[j].end();
    if (this_end > max_end) max_end = this_end;
  }
  return max_end;
}

MemRegion CardTableModRefBS::committed_unique_to_self(int self, 
                                                      MemRegion mr) const {
  MemRegion result = mr;
  for (int r = 0; r < _cur_covered_regions; r += 1) {
    if (r != self) {
      result = result.minus(_committed[r]);
    }
  }
  // Never include the guard page.
  result = result.minus(_guard_region);
  return result;
}

void CardTableModRefBS::resize_covered_region(MemRegion new_region) {
  // We don't change the start of a region, only the end.
  assert(_whole_heap.contains(new_region), 
	   "attempt to cover area not in reserved area");
  debug_only(verify_guard();)
  int ind = find_covering_region_by_base(new_region.start());
  MemRegion old_region = _covered[ind];
  assert(old_region.start() == new_region.start(), "just checking");
  if (new_region.word_size() != old_region.word_size()) {
    // Commit new or uncommit old pages, if necessary.
    MemRegion cur_committed = _committed[ind];
    // Extend the end of this _commited region 
    // to cover the end of any lower _committed regions.
    // This forms overlapping regions, but never interior regions.
    HeapWord* max_prev_end = largest_prev_committed_end(ind);
    if (max_prev_end > cur_committed.end()) {
      cur_committed.set_end(max_prev_end);
    }
    // Align the end up to a page size (starts are already aligned).
    jbyte* new_end = byte_after(new_region.last());
    HeapWord* new_end_aligned =
      (HeapWord*)align_size_up((uintptr_t)new_end, _page_size);
    assert(new_end_aligned >= (HeapWord*) new_end,
           "align up, but less");
    // The guard page is always committed and should not be committed over.
    HeapWord* new_end_for_commit = MIN2(new_end_aligned, _guard_region.start());
    if (new_end_for_commit > cur_committed.end()) {
      // Must commit new pages.
      MemRegion new_committed =
	MemRegion(cur_committed.end(), new_end_for_commit);

      assert(!new_committed.is_empty(), "Region should not be empty here");
      if (!os::commit_memory((char*)new_committed.start(),
	                     new_committed.byte_size(), Modules::CardTableModRefBS)) {
        // Do better than this for Merlin
        vm_exit_out_of_memory(new_committed.byte_size(),
	        "card table expansion");
      }
    // Use new_end_aligned (as opposed to new_end_for_commit) because
    // the cur_committed region may include the guard region.
    } else if (new_end_aligned < cur_committed.end()) {
      // Must uncommit pages.
      MemRegion uncommit_region = 
        committed_unique_to_self(ind, MemRegion(new_end_aligned,
                                                cur_committed.end()));
      if (!uncommit_region.is_empty()) {
os::uncommit_memory((char*)uncommit_region.start(),uncommit_region.byte_size(),Modules::CardTableModRefBS);
      }
    }
    // In any case, we can reset the end of the current committed entry.
    _committed[ind].set_end(new_end_aligned);

    // The default of 0 is not necessarily clean cards.
    jbyte* entry;
    if (old_region.last() < _whole_heap.start()) {
      entry = byte_for(_whole_heap.start());
    } else {
      entry = byte_after(old_region.last());
    }
    assert(index_for(new_region.last()) < (int) _guard_index,
      "The guard card will be overwritten");
    jbyte* end = byte_after(new_region.last());
    // do nothing if we resized downward.
    if (entry < end) {
      memset(entry, clean_card, pointer_delta(end, entry, sizeof(jbyte)));
    }
  }
  // In any case, the covered size changes.
  _covered[ind].set_word_size(new_region.word_size());
  if (TraceCardTableModRefBS) {
    gclog_or_tty->print_cr("CardTableModRefBS::resize_covered_region: ");
    gclog_or_tty->print_cr("  "
"  _covered[%d].start(): "PTR_FORMAT
"  _covered[%d].last(): "PTR_FORMAT,
                  ind, _covered[ind].start(), 
                  ind, _covered[ind].last());
    gclog_or_tty->print_cr("  "
"  _committed[%d].start(): "PTR_FORMAT
"  _committed[%d].last(): "PTR_FORMAT,
                  ind, _committed[ind].start(),
                  ind, _committed[ind].last());
    gclog_or_tty->print_cr("  "
"  byte_for(start): "PTR_FORMAT
"  byte_for(last): "PTR_FORMAT,
                  byte_for(_covered[ind].start()),
                  byte_for(_covered[ind].last()));
    gclog_or_tty->print_cr("  "
"  addr_for(start): "PTR_FORMAT
"  addr_for(last): "PTR_FORMAT,
                  addr_for((jbyte*) _committed[ind].start()),
                  addr_for((jbyte*) _committed[ind].last()));
  }
  debug_only(verify_guard();)
}

// Note that these versions are precise!  The scanning code has to handle the
// fact that the write barrier may be either precise or imprecise.

void CardTableModRefBS::write_ref_field_work(objectRef* field, objectRef newVal) {
  inline_write_ref_field(field, newVal);
}


void CardTableModRefBS::non_clean_card_iterate(Space* sp,
					       MemRegion mr,
					       DirtyCardToOopClosure* dcto_cl,
					       MemRegionClosure* cl,
					       bool clear) {
  if (!mr.is_empty()) {
    int n_threads = SharedHeap::heap()->n_par_threads();
    if (n_threads > 0) {
      fatal("Parallel gc not supported here.");
    } else {
      non_clean_card_iterate_work(mr, cl, clear);
    }
  }
}

// NOTE: For this to work correctly, it is important that
// we look for non-clean cards below (so as to catch those
// marked precleaned), rather than look explicitly for dirty
// cards (and miss those marked precleaned). In that sense,
// the name precleaned is currently somewhat of a misnomer.
void CardTableModRefBS::non_clean_card_iterate_work(MemRegion mr,
						    MemRegionClosure* cl,
						    bool clear) {
  // Figure out whether we have to worry about parallelism.
  bool is_par = (SharedHeap::heap()->n_par_threads() > 1);
  for (int i = 0; i < _cur_covered_regions; i++) {
    MemRegion mri = mr.intersection(_covered[i]);
    if (mri.word_size() > 0) {
      jbyte* cur_entry = byte_for(mri.last());
      jbyte* limit = byte_for(mri.start());
      while (cur_entry >= limit) {
        jbyte* next_entry = cur_entry - 1;
	if (*cur_entry != clean_card) {
	  size_t non_clean_cards = 1;
	  // Should the next card be included in this range of dirty cards.
          while (next_entry >= limit && *next_entry != clean_card) {
	    non_clean_cards++; 
	    cur_entry = next_entry;
	    next_entry--;
	  }
	  // The memory region may not be on a card boundary.  So that
	  // objects beyond the end of the region are not processed, make
	  // cur_cards precise with regard to the end of the memory region.
	  MemRegion cur_cards(addr_for(cur_entry), 
			      non_clean_cards * card_size_in_words);
	  MemRegion dirty_region = cur_cards.intersection(mri);
	  if (clear) {
            for (size_t i = 0; i < non_clean_cards; i++) {
	      // Clean the dirty cards (but leave the other non-clean
	      // alone.)  If parallel, do the cleaning atomically.
	      jbyte cur_entry_val = cur_entry[i];
	      if (card_is_dirty_wrt_gen_iter(cur_entry_val)) {
		if (is_par) {
		  jbyte res = Atomic::cmpxchg(clean_card, &cur_entry[i], cur_entry_val);
		  assert(res != clean_card,
			 "Dirty card mysteriously cleaned");
		} else {
		  cur_entry[i] = clean_card;
		}
	      }
            }
          }
	  cl->do_MemRegion(dirty_region);
	}
	cur_entry = next_entry;
      }
    }
  }
}

void CardTableModRefBS::mod_oop_in_space_iterate(Space* sp,
                                                 OopClosure* cl,
                                                 bool clear,
						 bool before_save_marks) {
  // Note that dcto_cl is resource-allocated, so there is no
  // corresponding "delete".
  DirtyCardToOopClosure* dcto_cl = sp->new_dcto_cl(cl, precision());
  MemRegion used_mr;
  if (before_save_marks) {
    used_mr = sp->used_region_at_save_marks();
  } else {
    used_mr = sp->used_region();
  }
  non_clean_card_iterate(sp, used_mr, dcto_cl, dcto_cl, clear);
}

void CardTableModRefBS::dirty_MemRegion(MemRegion mr) {
  jbyte* cur  = byte_for(mr.start());
  jbyte* last = byte_after(mr.last());
  while (cur < last) {
    *cur = dirty_card;
    cur++;
  }
}

void CardTableModRefBS::invalidate(MemRegion mr) {
  for (int i = 0; i < _cur_covered_regions; i++) {
    MemRegion mri = mr.intersection(_covered[i]);
    if (!mri.is_empty()) dirty_MemRegion(mri);
  }
}

void CardTableModRefBS::clear_MemRegion(MemRegion mr) {
  // Be conservative: only clean cards entirely contained within the
  // region.
  jbyte* cur;
  if (mr.start() == _whole_heap.start()) {
    cur = byte_for(mr.start());
  } else {
    assert(mr.start() > _whole_heap.start(), "mr is not covered.");
    cur = byte_after(mr.start() - 1);
  }
  jbyte* last = byte_after(mr.last());
  memset(cur, clean_card, pointer_delta(last, cur, sizeof(jbyte)));
}

void CardTableModRefBS::clear(MemRegion mr) {
  for (int i = 0; i < _cur_covered_regions; i++) {
    MemRegion mri = mr.intersection(_covered[i]);
    if (!mri.is_empty()) clear_MemRegion(mri);
  }
}

// NOTES:
// (1) Unlike mod_oop_in_space_iterate() above, dirty_card_iterate()
//     iterates over dirty cards ranges in increasing address order.
// (2) Unlike, e.g., dirty_card_range_after_preclean() below,
//     this method does not make the dirty cards prelceaned.
void CardTableModRefBS::dirty_card_iterate(MemRegion mr,
                                           MemRegionClosure* cl) {
  for (int i = 0; i < _cur_covered_regions; i++) {
    MemRegion mri = mr.intersection(_covered[i]);
    if (!mri.is_empty()) {
      jbyte *cur_entry, *next_entry, *limit;
      for (cur_entry = byte_for(mri.start()), limit = byte_for(mri.last());
           cur_entry <= limit;
           cur_entry  = next_entry) {
        next_entry = cur_entry + 1;
        if (*cur_entry == dirty_card) {
          size_t dirty_cards;
          // Accumulate maximal dirty card range, starting at cur_entry
          for (dirty_cards = 1;
               next_entry <= limit && *next_entry == dirty_card;
               dirty_cards++, next_entry++);
          MemRegion cur_cards(addr_for(cur_entry),
                              dirty_cards*card_size_in_words);
          cl->do_MemRegion(cur_cards);
        }
      }
    }
  }
}

MemRegion CardTableModRefBS::dirty_card_range_after_preclean(MemRegion mr) {
  for (int i = 0; i < _cur_covered_regions; i++) {
    MemRegion mri = mr.intersection(_covered[i]);
    if (!mri.is_empty()) {
      jbyte* cur_entry, *next_entry, *limit;
      for (cur_entry = byte_for(mri.start()), limit = byte_for(mri.last());
           cur_entry <= limit;
           cur_entry  = next_entry) {
        next_entry = cur_entry + 1;
        if (*cur_entry == dirty_card) {
          size_t dirty_cards;
          // Accumulate maximal dirty card range, starting at cur_entry
          for (dirty_cards = 1;
               next_entry <= limit && *next_entry == dirty_card;
               dirty_cards++, next_entry++);
          MemRegion cur_cards(addr_for(cur_entry),
                              dirty_cards*card_size_in_words);
          for (size_t i = 0; i < dirty_cards; i++) {
             cur_entry[i] = precleaned_card;
          }
          return cur_cards;
        }
      }
    }
  }
  return MemRegion(mr.end(), mr.end());
}

// Set all the dirty cards in the given region to "precleaned" state.
void CardTableModRefBS::preclean_dirty_cards(MemRegion mr) {
  for (int i = 0; i < _cur_covered_regions; i++) {
    MemRegion mri = mr.intersection(_covered[i]);
    if (!mri.is_empty()) {
      jbyte *cur_entry, *limit;
      for (cur_entry = byte_for(mri.start()), limit = byte_for(mri.last());
           cur_entry <= limit;
           cur_entry++) {
        if (*cur_entry == dirty_card) {
          *cur_entry = precleaned_card;
        }
      }
    }
  }
}

uintx CardTableModRefBS::ct_max_alignment_constraint() {
  return card_size * os::vm_page_size();
}

void CardTableModRefBS::verify_guard() {
  // For product build verification
  guarantee(_byte_map[_guard_index] == last_card,
            "card table guard has been modified");
}

void CardTableModRefBS::verify() {
  verify_guard();
}

#ifndef PRODUCT
class GuaranteeNotModClosure: public MemRegionClosure {
  CardTableModRefBS* _ct;
public:
  GuaranteeNotModClosure(CardTableModRefBS* ct) : _ct(ct) {}
  void do_MemRegion(MemRegion mr) {
    jbyte* entry = _ct->byte_for(mr.start());
    guarantee(*entry != CardTableModRefBS::clean_card,
	      "Dirty card in region that should be clean");
  }
};

void CardTableModRefBS::verify_clean_region(MemRegion mr) {
  GuaranteeNotModClosure blk(this);
  non_clean_card_iterate_work(mr, &blk, false);
}
#endif

bool CardTableModRefBSForCTRS::card_will_be_scanned(jbyte cv) {
  return
    CardTableModRefBS::card_will_be_scanned(cv) ||
    _rs->is_prev_nonclean_card_val(cv);
};

bool CardTableModRefBSForCTRS::card_may_have_been_dirty(jbyte cv) {
  return
    cv != clean_card &&
    (CardTableModRefBS::card_may_have_been_dirty(cv) ||
     CardTableRS::youngergen_may_have_been_dirty(cv));
};
