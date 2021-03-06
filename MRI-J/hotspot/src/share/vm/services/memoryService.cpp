/*
 * Copyright 2003-2006 Sun Microsystems, Inc.  All Rights Reserved.
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


#include "classLoadingService.hpp"
#include "codeCache.hpp"
#include "collectedHeap.hpp"
#include "compactingPermGenGen.hpp"
#include "genCollectedHeap.hpp"
#include "generationSpec.hpp"
#include "gpgc_heap.hpp"
#include "instanceKlass.hpp"
#include "javaCalls.hpp"
#include "lowMemoryDetector.hpp"
#include "management.hpp"
#include "memoryManager.hpp"
#include "memoryPool.hpp"
#include "memoryService.hpp"
#include "parallelScavengeHeap.hpp"
#include "permGen.hpp"
#include "psMemoryPool.hpp"
#include "safepoint.hpp"
#include "tickProfiler.hpp"
#include "universe.hpp"
#include "vmSymbols.hpp"

#include "allocation.inline.hpp"
#include "atomic_os_pd.inline.hpp"
#include "mutex.inline.hpp"
#include "oop.inline.hpp"
#include "thread_os.inline.hpp"

GrowableArray<MemoryPool*>* MemoryService::_pools_list = new (ResourceObj::C_HEAP) GrowableArray<MemoryPool*>(init_pools_list_size, true);
GrowableArray<MemoryManager*>* MemoryService::_managers_list = new (ResourceObj::C_HEAP) GrowableArray<MemoryManager*>(init_managers_list_size, true);

GCMemoryManager* MemoryService::_minor_gc_manager = NULL;
GCMemoryManager* MemoryService::_major_gc_manager = NULL;
MemoryPool*MemoryService::_code_cache_pool=NULL;


void memoryService_init() {
  MemoryService::set_universe_heap(Universe::heap());
}


class GcThreadCountClosure: public ThreadClosure {
 private:
  int _count;
 public:
  GcThreadCountClosure() : _count(0) {};
  void do_thread(Thread* thread);
  int count() { return _count; }
};

void GcThreadCountClosure::do_thread(Thread* thread) {
  _count++;
}

void MemoryService::set_universe_heap(CollectedHeap* heap) {
  CollectedHeap::Name kind = heap->kind();
  switch (kind) {
    case CollectedHeap::GenCollectedHeap : {
      add_gen_collected_heap_info(GenCollectedHeap::heap());
      break;
    }
    case CollectedHeap::ParallelScavengeHeap : {
      add_parallel_scavenge_heap_info(ParallelScavengeHeap::heap());
      break;
    }
    case CollectedHeap::GenPauselessHeap :
      add_gen_pauseless_heap_info(GPGC_Heap::heap());
      break;
    default:
      guarantee(false, "Not recognized kind of heap");
  }

  // set the GC thread count
  GcThreadCountClosure gctcc;
  heap->gc_threads_do(&gctcc);
  int count = gctcc.count();
  if (count > 0) {
    _minor_gc_manager->set_num_gc_threads(count);
    _major_gc_manager->set_num_gc_threads(count);
  }

  // All memory pools and memory managers are initialized.
  //
  _minor_gc_manager->initialize_gc_stat_info();
  _major_gc_manager->initialize_gc_stat_info();
}

// Add memory pools for GenCollectedHeap
// This function currently only supports two generations collected heap.
// The collector for GenCollectedHeap will have two memory managers.
void MemoryService::add_gen_collected_heap_info(GenCollectedHeap* heap) {
  CollectorPolicy* policy = heap->collector_policy();

  assert(policy->is_two_generation_policy(), "Only support two generations");
  guarantee(heap->n_gens() == 2, "Only support two-generation heap");

  if (policy->is_mark_sweep_policy()) {
    _minor_gc_manager = MemoryManager::get_copy_memory_manager();
    _major_gc_manager = MemoryManager::get_msc_memory_manager();
  } 
  _managers_list->append(_minor_gc_manager);
  _managers_list->append(_major_gc_manager);

  add_generation_memory_pool(heap->get_gen(minor), _major_gc_manager, _minor_gc_manager);
  add_generation_memory_pool(heap->get_gen(major), _major_gc_manager);

  PermGen::Name name = policy->permanent_generation()->name();
  switch (name) {
    case PermGen::MarkSweepCompact: {
      CompactingPermGenGen* perm_gen = (CompactingPermGenGen*) heap->perm_gen();
      add_compact_perm_gen_memory_pool(perm_gen, _major_gc_manager);
      break;
    }
    default:
      guarantee(false, "Unrecognized perm generation");
        break;
  }
}

// Add memory pools for ParallelScavengeHeap 
// This function currently only supports two generations collected heap.
// The collector for ParallelScavengeHeap will have two memory managers.
void MemoryService::add_parallel_scavenge_heap_info(ParallelScavengeHeap* heap) {
  // Two managers to keep statistics about _minor_gc_manager and _major_gc_manager GC.
  _minor_gc_manager = MemoryManager::get_psScavenge_memory_manager();
  _major_gc_manager = MemoryManager::get_psMarkSweep_memory_manager();
  _managers_list->append(_minor_gc_manager);
  _managers_list->append(_major_gc_manager);

  add_psYoung_memory_pool(heap->young_gen(), _major_gc_manager, _minor_gc_manager);
  add_psOld_memory_pool(heap->old_gen(), _major_gc_manager);
  add_psPerm_memory_pool(heap->perm_gen(), _major_gc_manager);
}

MemoryPool* MemoryService::add_gen(Generation* gen, 
                                   const char* name, 
                                   bool is_heap,
                                   bool support_usage_threshold) {
 
  MemoryPool::PoolType type = (is_heap ? MemoryPool::Heap : MemoryPool::NonHeap);
  GenerationPool* pool = new GenerationPool(gen, name, type, support_usage_threshold);
  _pools_list->append(pool);
  return (MemoryPool*) pool;
}

MemoryPool* MemoryService::add_space(ContiguousSpace* space,
                                     const char* name,   
                                     bool is_heap,
                                     size_t max_size,
                                     bool support_usage_threshold) {
  MemoryPool::PoolType type = (is_heap ? MemoryPool::Heap : MemoryPool::NonHeap);
  ContiguousSpacePool* pool = new ContiguousSpacePool(space, name, type, max_size, support_usage_threshold);
 
  _pools_list->append(pool);
  return (MemoryPool*) pool;
}

MemoryPool* MemoryService::add_survivor_spaces(DefNewGeneration* gen,
                                               const char* name,   
                                               bool is_heap,
                                               size_t max_size,
                                               bool support_usage_threshold) {
  MemoryPool::PoolType type = (is_heap ? MemoryPool::Heap : MemoryPool::NonHeap);
  SurvivorContiguousSpacePool* pool = new SurvivorContiguousSpacePool(gen, name, type, max_size, support_usage_threshold);
 
  _pools_list->append(pool);
  return (MemoryPool*) pool;
}

// Add memory pool(s) for one generation
void MemoryService::add_generation_memory_pool(Generation* gen, 
                                               MemoryManager* major_mgr,
                                               MemoryManager* minor_mgr) {
  Generation::Name kind = gen->kind();
  int index = _pools_list->length();

  switch (kind) {
    case Generation::DefNew: {
      assert(major_mgr != NULL && minor_mgr != NULL, "Should have two managers");
      DefNewGeneration* young_gen = (DefNewGeneration*) gen;
      // Add a memory pool for each space and young gen doesn't 
      // support low memory detection as it is expected to get filled up.
      MemoryPool* eden = add_space(young_gen->eden(),
                                   "Eden Space",
                                   true, /* is_heap */
                                   young_gen->max_eden_size(),
                                   false /* support_usage_threshold */);
      MemoryPool* survivor = add_survivor_spaces(young_gen, 
                                                 "Survivor Space", 
                                                 true, /* is_heap */
                                                 young_gen->max_survivor_size(),
                                                 false /* support_usage_threshold */);
      break;
    }

    case Generation::MarkSweepCompact: {
      assert(major_mgr != NULL && minor_mgr == NULL, "Should have only one manager");
      add_gen(gen,
              "Tenured Gen",
              true, /* is_heap */
              true  /* support_usage_threshold */);
      break;
    }

    default:
      assert(false, "should not reach here");
      // no memory pool added for others
      break;
  }

  assert(major_mgr != NULL, "Should have at least one manager");
  // Link managers and the memory pools together
  for (int i = index; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    major_mgr->add_pool(pool);
    if (minor_mgr != NULL) {
      minor_mgr->add_pool(pool);
    }
  }
}

void MemoryService::add_compact_perm_gen_memory_pool(CompactingPermGenGen* perm_gen,
                                                     MemoryManager* mgr) {
  PermanentGenerationSpec* spec = perm_gen->spec();
size_t max_size=spec->max_size();
  MemoryPool* pool = add_space(perm_gen->unshared_space(),
                               "Perm Gen", 
                                false, /* is_heap */
                                max_size,
                                true   /* support_usage_threshold */);
  mgr->add_pool(pool);
}

void MemoryService::add_psYoung_memory_pool(PSYoungGen* gen, MemoryManager* major_mgr, MemoryManager* minor_mgr) {
  assert(major_mgr != NULL && minor_mgr != NULL, "Should have two managers");

  // Add a memory pool for each space and young gen doesn't 
  // support low memory detection as it is expected to get filled up.
  EdenMutableSpacePool* eden = new EdenMutableSpacePool(gen,
                                                        gen->eden_space(), 
                                                        "PS Eden Space", 
                                                        MemoryPool::Heap,
                                                        false /* support_usage_threshold */);

  SurvivorMutableSpacePool* survivor = new SurvivorMutableSpacePool(gen,
                                                                    "PS Survivor Space", 
                                                                    MemoryPool::Heap,
                                                                    false /* support_usage_threshold */);

  major_mgr->add_pool(eden);
  major_mgr->add_pool(survivor);
  minor_mgr->add_pool(eden);
  minor_mgr->add_pool(survivor);
  _pools_list->append(eden);
  _pools_list->append(survivor);
}

void MemoryService::add_psOld_memory_pool(PSOldGen* gen, MemoryManager* mgr) {
  PSGenerationPool* old_gen = new PSGenerationPool(gen, 
                                                   "PS Old Gen",
                                                   MemoryPool::Heap, 
                                                   true /* support_usage_threshold */);
  mgr->add_pool(old_gen);
  _pools_list->append(old_gen);
}

void MemoryService::add_psPerm_memory_pool(PSPermGen* gen, MemoryManager* mgr) {
  PSGenerationPool* perm_gen = new PSGenerationPool(gen, 
                                                    "PS Perm Gen", 
                                                    MemoryPool::NonHeap,
                                                    true /* support_usage_threshold */);
  mgr->add_pool(perm_gen);
  _pools_list->append(perm_gen);
}

// GenPauselessGC support
void MemoryService::add_gen_pauseless_memory_pool(GPGC_Generation*gen,
                                                  const char* name,
                                                  MemoryManager* major_mgr,
                                                  MemoryManager* minor_mgr) {
  GenPauselessHeapPool* pool = new GenPauselessHeapPool(gen, name, MemoryPool::Heap,
                                       GPGC_Heap::heap()->max_heap_size_specified(),
                                       true   /* support_usage_threshold */);

major_mgr->add_pool(pool);

  if (minor_mgr != NULL) {
    minor_mgr->add_pool(pool);
  }

_pools_list->append(pool);
}

// Add memory pools for GenPauselessHeap 
void MemoryService::add_gen_pauseless_heap_info(GPGC_Heap*heap){
_minor_gc_manager=MemoryManager::get_gpgc_newgc_memory_manager();
_major_gc_manager=MemoryManager::get_gpgc_oldgc_memory_manager();
  _managers_list->append(_minor_gc_manager);
  _managers_list->append(_major_gc_manager);

  add_gen_pauseless_memory_pool(heap->new_gen(),  "GenPauseless New Gen",  _major_gc_manager, _minor_gc_manager);
  add_gen_pauseless_memory_pool(heap->old_gen(),  "GenPauseless Old Gen",  _major_gc_manager, NULL);
  add_gen_pauseless_memory_pool(heap->perm_gen(), "GenPauseless Perm Gen", _major_gc_manager, NULL);
}

void MemoryService::add_code_cache_memory_pool(){
  _code_cache_pool = new CodeCachePool();
  MemoryManager* mgr = MemoryManager::get_code_cache_memory_manager();
mgr->add_pool(_code_cache_pool);

_pools_list->append(_code_cache_pool);
  _managers_list->append(mgr);
}

MemoryManager* MemoryService::get_memory_manager(instanceHandle mh) {
  for (int i = 0; i < _managers_list->length(); i++) {
    MemoryManager* mgr = _managers_list->at(i);
    if (mgr->is_manager(mh)) {
      return mgr;
    }
  }
  return NULL;
}

MemoryPool* MemoryService::get_memory_pool(instanceHandle ph) {
  for (int i = 0; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    if (pool->is_pool(ph)) {
      return pool;
    }
  }
  return NULL;
}

void MemoryService::track_memory_usage() {
  // Track the peak memory usage
  for (int i = 0; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    pool->record_peak_memory_usage();
  }

  // Detect low memory
  LowMemoryDetector::detect_low_memory();
}

void MemoryService::track_memory_pool_usage(MemoryPool* pool) {
  // Track the peak memory usage
  pool->record_peak_memory_usage();
  
  // Detect low memory
  if (LowMemoryDetector::is_enabled(pool)) {
    LowMemoryDetector::detect_low_memory(pool);
  }
}

void MemoryService::gc_begin(bool fullGC) {
  GCMemoryManager* mgr; 
  if (fullGC) {
    mgr = _major_gc_manager;
  } else {
    mgr = _minor_gc_manager;
  }
  assert(mgr->is_gc_memory_manager(), "Sanity check");
  mgr->gc_begin();

  // Track the peak memory usage when GC begins
  for (int i = 0; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    pool->record_peak_memory_usage();
  }
}

void MemoryService::gc_end(bool fullGC) {
  GCMemoryManager* mgr; 
  if (fullGC) {
    mgr = (GCMemoryManager*) _major_gc_manager;
  } else {
    mgr = (GCMemoryManager*) _minor_gc_manager;
  }
  assert(mgr->is_gc_memory_manager(), "Sanity check");

  // register the GC end statistics and memory usage
  mgr->gc_end();
}

void MemoryService::oops_do(OopClosure* f) {
  int i;

  for (i = 0; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    pool->oops_do(f);
  }
  for (i = 0; i < _managers_list->length(); i++) {
    MemoryManager* mgr = _managers_list->at(i);
    mgr->oops_do(f);
  }
}

bool MemoryService::set_verbose(bool verbose) {
  MutexLockerAllowGC m(Management_lock,JavaThread::current());
  // verbose will be set to the previous value
  bool succeed = CommandLineFlags::boolAtPut((char*)"PrintGC", &verbose, MANAGEMENT);
  assert(succeed, "Setting PrintGC flag fails");
  ClassLoadingService::reset_trace_class_unloading(); 

  return verbose;
}

Handle MemoryService::create_MemoryUsage_obj(MemoryUsage usage, TRAPS) {
  klassOop k = Management::java_lang_management_MemoryUsage_klass(CHECK_NH);
  instanceKlassHandle ik(THREAD, k);

  instanceHandle obj = ik->allocate_instance_handle(false/* SBA */, CHECK_NH);

  JavaValue result(T_VOID);
  JavaCallArguments args(10);
  args.push_oop(obj);                         // receiver
  args.push_long(usage.init_size_as_jlong()); // Argument 1
  args.push_long(usage.used_as_jlong());      // Argument 2
  args.push_long(usage.committed_as_jlong()); // Argument 3
  args.push_long(usage.max_size_as_jlong());  // Argument 4

  JavaCalls::call_special(&result,
                          ik,
                          vmSymbolHandles::object_initializer_name(),
                          vmSymbolHandles::long_long_long_long_void_signature(),
                          &args,
                          CHECK_NH);
  return obj;
}
//
// GC manager type depends on the type of Generation. Depending the space
// availablity and vm option the gc uses major gc manager or minor gc 
// manager or both. The type of gc manager depends on the generation kind. 
// For DefNew, ParNew and ASParNew generation doing scavange gc uses minor 
// gc manager (so _fullGC is set to false ) and for other generation kind 
// DOing mark-sweep-compact uses major gc manager (so _fullGC is set 
// to true).
TraceMemoryManagerStats::TraceMemoryManagerStats(Generation::Name kind) {
  switch (kind) {
    case Generation::DefNew:
    case Generation::ParNew: 
      _fullGC=false;
      break;
    case Generation::MarkSweepCompact:
      _fullGC=true;
      break;
    default:
      assert(false, "Unrecognized gc generation kind.");
  }
  MemoryService::gc_begin(_fullGC);
}
TraceMemoryManagerStats::TraceMemoryManagerStats(bool fullGC) {
  _fullGC = fullGC;
  MemoryService::gc_begin(_fullGC);
}

TraceMemoryManagerStats::~TraceMemoryManagerStats() {
  MemoryService::gc_end(_fullGC);
}
