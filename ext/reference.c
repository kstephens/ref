/**********************************************************************

  reference.c - Reference API for MRI.

  Author: Kurt Stephens
  created at: Mon Jan 17 12:09:32 CST 2011

  Copyright (C) 2011 Kurt Stephens, Enova Financial

**********************************************************************/
#include "ruby.h"
#ifdef RUBY_RUBY_H /* ! MRI 1.8 */
#include "ruby/gc_api.h"
#else
#include "gc_api.h"
#include "st.h"
typedef int st_index_t;
#define SIZET2NUM(X) ULONG2NUM(X) /* might not be portable to 64-bit. */
#endif
#include <assert.h>

#ifndef REFERENCE_DEBUG
#define REFERENCE_DEBUG 0
#endif

#ifndef SOFT_REFERENCE_DEBUG
#define SOFT_REFERENCE_DEBUG 0
#endif

#ifndef REFERENCE_QUEUE_DEBUG
#define REFERENCE_QUEUE_DEBUG REFERENCE_DEBUG
#endif

#ifndef CACHE_OBJECT_ID
#define CACHE_OBJECT_ID 0
#endif

#ifndef REF_TRACE
#define REF_TRACE 1
#endif

#ifndef REF_FREE_LIST
#define REF_FREE_LIST 0
#endif

static VALUE rb_mReference;
static VALUE rb_cReference;
static VALUE rb_cWeakReference;
static VALUE rb_cSoftReference;
static ID id_object, id_object_id;
static ID 
  id__mri_notify_reference_queues,
  id__mri_push_reference, 
  id__mri_memory_pressure_occurred,
  id__mri_decrement_gc_left,
  id__mri_after_gc;

/*********************************************************************
 * Reference tables.
 *
 * * Key is the WeakReference or SoftReference subclass Class object VALUE.
 * * Value is an st_table* containing:
 * ** Key is a referenced (non-immediate) Object VALUE.
 * ** Value is a Reference object VALUE.
 */
static st_table *weak_table = 0;
static st_table *soft_table = 0;

static int value_compare ( st_data_t a, st_data_t b )
{
  return ! a == b;
}

static st_index_t value_hash ( st_data_t a )
{
  return (st_index_t) a;
}

static struct st_hash_type ref_table_type = {
  value_compare,
  value_hash
};

/********************************************************************/

struct ref_table_func {
  int (*func)(ANYARGS);
  st_data_t data;
};

static int
ref_table_count(st_data_t _cls, st_data_t _ref_table, st_data_t data)
{
  VALUE cls = (VALUE) _cls;
  st_table *ref_table = (st_table*) _ref_table;
  (* (size_t*) data) += ref_table->num_entries;
  return ST_CONTINUE;
}

static int 
ref_table_foreach_(st_data_t _cls, st_data_t _ref_table, st_data_t data)
{
  VALUE cls = (VALUE) _cls;
  st_table *ref_table = (st_table*) _ref_table;
  struct ref_table_func *func = (void*) data;
  st_foreach(ref_table, func->func, func->data);
  return ST_CONTINUE;
}

static int
ref_table_foreach(st_table *t, int (*func)(ANYARGS), st_data_t data)
{
  struct ref_table_func func_ = { func, data };
  return st_foreach(t, ref_table_foreach_, (st_data_t) &func_);
}

static st_table *
ref_table_for_cls(st_table *base_table, VALUE cls)
{
  st_table *ref_table;
  st_data_t value = 0;
  if ( st_lookup(base_table, (st_data_t) cls, &value) ) {
    ref_table = (st_table*) value;
  } else {
    ref_table = st_init_table(&ref_table_type);
    st_insert(base_table, (st_data_t) cls, (st_data_t) ref_table);
  }
  return ref_table;
}

/* Call only before sweep. */
static int 
remove_dead_reference_class(st_data_t _cls, st_data_t _ref_table, st_data_t data)
{
  VALUE cls = (VALUE) _cls;
  st_table *ref_table = (st_table*) _ref_table;
  if ( ! RB_GC_MARKED(cls) ) {
    assert(ref_table->num_entries == 0);
    st_free_table(ref_table);
    ++ * (size_t*) data;
    return ST_DELETE;
  }
  return ST_CONTINUE;
}

/* Call only before sweep. */
static void
remove_dead_reference_classes(void *callback, void *func_data)
{
  size_t n_released = 0;
  size_t n_before = weak_table->num_entries + soft_table->num_entries;

  st_foreach(weak_table, remove_dead_reference_class, (st_data_t) &n_released);
  st_foreach(soft_table, remove_dead_reference_class, (st_data_t) &n_released);

#if 0
  if ( n_released )
  fprintf(stderr, "  remove_dead_reference_classes(): %lu before, %lu removed, %lu alive.\n\n", 
	  (unsigned long) n_before,
	  (unsigned long) n_released,
	  (unsigned long) weak_table->num_entries + soft_table->num_entries 
	  );
#endif
}

/*********************************************************************
 * Document-class: Reference::Reference
 *
 * Base class for References.
 */


/*********************************************************************
 * Reference private data.
 */
typedef struct rb_reference {
  VALUE object;           /* Reference#object */
  VALUE ref_object;       /* Reference object */
#if CACHE_OBJECT_ID
  VALUE object_id;        /* May be a non-immediate. */
#endif
  st_table *ref_table;    /* The ref table this belongs to. */
  struct rb_ref_queue_list *ref_queues; /* List of WeakReference objects pointing to ReferenceQueue objects. */
  unsigned int traversed_since_gc : 1;
  unsigned int gc_ttl : 15;
  unsigned int notify_ref_queue : 1;
  unsigned int gc_left : 15;
  size_t ref_id;          /* May wrap back to 0. */
  struct rb_reference *next; /* For free list or queued_refs. */
} rb_reference;

typedef struct rb_ref_queue_list {
  VALUE ref_queue;
  struct rb_ref_queue_list *next;
} rb_ref_queue_list;

static rb_reference *queued_refs;
static size_t queued_refs_count = 0;

static size_t n_refs_live; /* total number of References objects alive now. */
static size_t n_refs; /* total number of References objects ever created. */

#if REF_FREE_LIST
static rb_reference *ref_free_list;
#endif

void
ref_unexpected()
{
  1 + 1;
  /* NOTHING */
}

static unsigned long gc_count = 0;

static int ref_trace = 0;
#if REF_TRACE
static rb_reference*
_rb_reference_check(void *ptr, const char *function_name, const char *file, int line)
{
  rb_reference *ref = ptr;
  int trace_this = ref_trace;
#if 0
  {
    static size_t stop_at_ref_ids[] = { -1, 0 };
    if ( ! trace_this ) {
      int i;
      for ( i = 0; stop_at_ref_ids[i]; ++ i ) {
	if ( stop_at_ref_ids[i] == ref->ref_id ) {
	  trace_this = 1;
	  break;
	}
      }
    }
  }
#endif

  if ( trace_this ) {
    fprintf(stderr, "  ## gc %lu: %32s(ref %p ref_id=%ld): object %p%s ref_obj %p%s ref_queues %p %s%s\n",
	    (unsigned long) gc_count,
	    function_name, (void*) ref, (unsigned long) ref->ref_id, 
	    (void*) ref->object, (RB_GC_MARKED(ref->object) ? "#" : " "), 
	    (void*) ref->ref_object, (RB_GC_MARKED(ref->ref_object) ? "#" : " "),
	    (void*) ref->ref_queues, (ref->notify_ref_queue ? "NOTIFY" : ""),
	    "");
    ref_unexpected();
  }

  return ref;
}
#define RB_REFERENCE_CHECK(X) _rb_reference_check((X), __FUNCTION__, __FILE__, __LINE__)
#else
#define RB_REFERENCE_CHECK(X) ((rb_reference*) (X))
#endif

static int 
ref_yield(st_data_t obj, st_data_t ref_obj, st_data_t data)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  rb_yield(ref->ref_object);
  return ST_CONTINUE;
}

static void 
ref_free(void *ptr)
{
  rb_reference *ref = RB_REFERENCE_CHECK(ptr);

#if REFERENCE_DEBUG > 3
  fprintf(stderr, "  ref_free(ref %p): ref_table %p, ref_queues = %p N %ld\n", (void*) ref, (void*) ref->ref_table, (void*) ref->ref_queues, (unsigned long) n_refs_live);
#endif

  assert(n_refs_live);
  -- n_refs_live;

  if ( ref->ref_table ) {
    if ( FL_ABLE(ref->object) ) {
      st_data_t key = (st_data_t) ref->object, value = 0;
      if ( ! st_delete(ref->ref_table, &key, &value) ) {
#if REFERENCE_DEBUG > 4
	fprintf(stderr, "    ref %p, object %p, ref_obj %p NOT DELETED from ref_table %p:\n      key=%p value=%p\n", 
		(void*) ref, (void*) ref->object, (void*) ref->ref_object, (void*) ref->ref_table,
		(void*) key, (void*) value);
	ref_unexpected();
#endif
      } else {
#if REFERENCE_DEBUG > 4
	fprintf(stderr, "    ref %p, object %p, ref_obj %p deleted from ref_table %p:\n      key=%p value=%p\n", 
		(void*) ref, (void*) ref->object, (void*) ref->ref_object, (void*) ref->ref_table,
		(void*) key, (void*) value);
#endif
      }
    }
    ref->ref_table = 0;
  }
  ref->ref_object = ref->object = 
#if CACHE_OBJECT_ID
    ref->object_id = 
#endif
    Qnil;
  ref->notify_ref_queue = 0;
  {
    rb_ref_queue_list *ref_queues;
    while ( (ref_queues = ref->ref_queues) ) {
      ref_queues->ref_queue = Qnil;
      ref->ref_queues = ref_queues->next;
      xfree(ref_queues);
    }
  }
#if REF_FREE_LIST
  ref->next = ref_free_list;
  ref_free_list = ref;
#else
  ref->next = 0;
  xfree(ptr);
#endif
}

static void
weak_free(void *ptr)
{
  ref_free(ptr);
}

static void
soft_free(void *ptr)
{
  ref_free(ptr);
}

static void
ref_mark(void *ptr)
{
  rb_reference *ref = RB_REFERENCE_CHECK(ptr);

#if REFERENCE_DEBUG > 3
  fprintf(stderr, "  ref_mark(ref %p): ref_obj %p marked=%d\n", 
	  (void*) ref, (void*) ref->ref_object, (int) RB_GC_MARKED(ref->ref_object));
#endif

#if CACHE_OBJECT_ID
  rb_gc_mark(ref->object_id); /* may not be immediate. */
#endif

  {
    rb_ref_queue_list *rql;
    for ( rql = ref->ref_queues; rql; rql = rql->next ) {
      rb_gc_mark(rql->ref_queue);
    }
  }
}

static void
soft_mark(void *ptr)
{
  rb_reference *ref = RB_REFERENCE_CHECK(ptr);
  ref_mark(ptr);
  if ( ref->gc_left > 0 )
    rb_gc_mark(ref->object);
}

#define FL_FLAGS(x) (FL_ABLE(x) ? RBASIC(x)->flags : 0)


size_t dereference_count;

/* Called when Reference#object no longer reachable. */
static int
ref_dereference(rb_reference *ref)
{
  ref = RB_REFERENCE_CHECK(ref);

  ++ dereference_count;

  /* Already dereferenced. */
  if ( ref->object == Qnil 
#if CACHE_OBJECT_ID
       && ref->object_id == Qnil
#endif
       ) {
#if REFERENCE_DEBUG >= 3
    fprintf(stderr, "      ref_dereference(ref %p ref_id %lu): ALREADY DEREFERENCED\n", (void*) ref, (unsigned long) ref->ref_id);
#endif
    ref->ref_table = 0;
    return ST_DELETE;
  }

#if REFERENCE_DEBUG >= 3
  fprintf(stderr, "      ref_dereference(ref %p)\n", (void*) ref);
  fprintf(stderr, "        FL_FLAGS(object %p) => %08x\n",  (void*) ref->object,     (int) FL_FLAGS(ref->object));
  fprintf(stderr, "        FL_FLAGS(ref_obj %p) => %08x\n", (void*) ref->ref_object, (int) FL_FLAGS(ref->ref_object));
#endif

  ref->object = 
#if CACHE_OBJECT_ID
    ref->object_id =
#endif
    Qnil;
  ref->traversed_since_gc = ref->gc_left = ref->gc_ttl = 0;

  /* If any ReferenceQueue was listening... */
  if ( ref->ref_queues ) {
    ref->notify_ref_queue = 1;
    ref->next = queued_refs;
    queued_refs = ref;
    ++ queued_refs_count;
    assert(queued_refs_count);

#if REFERENCE_QUEUE_DEBUG > 2
    fprintf(stderr, "        has ref_queues %p scheduled %d\n", 
	    (void*) ref->ref_queues, (int) queued_refs_count);
#endif
  }

  /* Remove from ref table. */
  ref->ref_table = 0;
  return ST_DELETE;
}

static int soft_gc_ttl = 10;

static VALUE m_reference_add_reference_queue(VALUE ref_obj, VALUE ref_queue);

static VALUE 
ref_new(st_table *base_table, VALUE cls, VALUE object, VALUE ref_queue, void *markf, void *freef, int gc_ttl)
{
  VALUE ref_obj = 0;
  st_table *ref_table = ref_table_for_cls(base_table, cls);
  if ( ! (FL_ABLE(object) && st_lookup(ref_table, (st_data_t) object, (st_data_t*) &ref_obj)) ) {
    rb_reference *ref;
#if REF_FREE_LIST
    if ( ref_free_list ) {
      ref = ref_free_list;
      ref_free_list = ref->next;
    } else
#endif
    ref = xmalloc(sizeof(*ref));

    ref->object = object;
#if CACHE_OBJECT_ID
    ref->object_id = rb_obj_id(object);
#endif
    ref->ref_table = 0;
    ref->traversed_since_gc = 0;
    ref->ref_queues = 0;
    ref->notify_ref_queue = 0;
    ref->gc_left = ref->gc_ttl = gc_ttl;
    ref->ref_id = ++ n_refs;
#if REF_FREE_LIST
    ref->next = 0;
#endif
    ref_obj = ref->ref_object = rb_data_object_alloc(cls, ref, markf, freef);    

    ++ n_refs_live;
    assert(n_refs_live);

    /* Avoid caching References to immediates. */
    if ( FL_ABLE(object) )
      st_insert(ref->ref_table = ref_table, (st_data_t) object, (st_data_t) ref_obj);

#if REFERENCE_DEBUG > 2
    fprintf(stderr, "  ref_new(cls %p, object %p) => ref %p, ref_obj %p in table %p N %ld\n", 
	    (void*) cls, (void*) object, 
	    (void*) ref, (void*) ref_obj, (void*) ref->ref_table, (unsigned long) n_refs_live );
#endif

    ref = RB_REFERENCE_CHECK(ref);
  }
  if ( ref_queue != Qnil ) {
    m_reference_add_reference_queue(ref_obj, ref_queue);
  }
  return ref_obj;
}

/*
 * Document-class: Reference::Reference
 *
 * call-seq:
 * Reference._mri_live_instance_count
 *
 * Return the number of live Reference objects.
 */
static VALUE
M_reference_live_instance_count(VALUE cls)
{
  return SIZET2NUM(n_refs_live);
}

/* call-seq:
 * Reference#referenced_object_id
 *
 * Returns the referenced object's object_id or nil if the object is no longer reachable.
 */
static VALUE 
m_reference_referenced_object_id(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
#if CACHE_OBJECT_ID
  return ref->object_id;
#else
  return ref->object == Qnil ? Qnil : rb_obj_id(ref->object);
#endif
}

/* call-seq:
 * Reference#_mri_reference_id.
 *
 * Returns the Reference object's id.
 */
static VALUE 
m_reference_reference_id(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  return SIZET2NUM(ref->ref_id);
}

/* call-seq:
 * Reference#object
 *
 * Returns the referenced object or nil if the object is no longer reachable.
 */
static VALUE 
m_reference_object(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  return ref->object;
}

/* call-seq:
 * Reference#_mri_object=
 *
 * Sets the reference object.
 */
static VALUE
m_reference_object_set(VALUE ref_obj, VALUE object)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  ref->object = object;
#if CACHE_OBJECT_ID
  ref->object_id = rb_obj_id(object);
#endif
  return ref_obj;  
}

/* call-seq:
 * Reference#_mri_dereference
 *
 * Clears #object and schedules notification of ReferenceQueues.
 */
static VALUE
m_reference_dereference(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  VALUE object = ref->object;
  st_table *ref_table = ref->ref_table; /* maybe zeroed by ref_dereference(). */
  if ( ref_dereference(ref) == ST_DELETE ) {
#if 0
    st_data_t key = (st_data_t) object, value = 0;
    st_delete(ref_table, &key, &value);
#endif
  }
  return ref_obj;
}

static VALUE
M_weak_new(int argc, VALUE *argv, VALUE cls);

/* call-seq:
 * Reference#_mri_add_reference_queue(ref_queue)
 *
 * Adds ref_queue to a Reference as a ReferenceQueue.
 * queue#push(Reference) is called when the Reference#object is no longer reachable.
 * A queue is only registered once per Reference.
 * Adding nil, removes any unreachable reference queues for this Reference.
 */
static VALUE 
m_reference_add_reference_queue(VALUE ref_obj, VALUE ref_queue)
{
  /* NOT native thread-safe. */
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  VALUE ref_queue_weak;

  /* Do not add reference queues to References to immediates. */
  if ( ! FL_ABLE(ref->object) )
    ref_queue = Qnil;

  /*
   * Make a WeakReference to the reference queue.
   * This avoids keeping unused reference queues pinned when References out live their reference queues.
   */
  ref_queue_weak = ref_queue == Qnil ? Qnil : M_weak_new(1, &ref_queue, rb_cWeakReference);

  /* Only add the ReferenceQueue once. */
  {
    rb_ref_queue_list *rql, **rql_prev, *unused = 0;
    
    rql_prev = &ref->ref_queues;
    while ( (rql = *rql_prev) ) {
      VALUE object;

#if 0
      fprintf(stderr, "   rql: SCAN ref %p %lu for ref_queue %p in rql %p %p\n", 
	      (void*) ref, (unsigned long) ref->ref_id,
	      (void*) ref_queue_weak,
	      (void*) rql, (void*) rql->ref_queue);
#endif

      /* Check via weak cache table. */
      if ( rql->ref_queue == ref_queue_weak ) 
	return ref_obj;

      /* Traverse WeakReference to ReferenceQueue. */
      object = rql->ref_queue;
      if ( object != Qnil )
	object = rb_funcall(object, id_object, 0);

      /* Check for collected weak refs. */
      if ( object == Qnil ) {
	/* Remove it from list. */
	/*
	  +-----------+     +-----------------+     +--------------+
	  | *rql_prev |---->| data   |        |---->|              |
	  +-----------+     +-----------------+     +--------------+
                            ^
	                    rql

                          +----------------------+
	  +-----------+  |  +-----------------+  |  +--------------+
	  | *rql_prev |--+  | data   |        |--+->|              |
	  +-----------+     +-----------------+     +--------------+
                                                    ^
	                                            rql
	 */
#if 0
	fprintf(stderr, "   rql: DELE ref %p %lu for ref_queue %p in rql %p %p\n", 
		(void*) ref, (unsigned long) ref->ref_id,
		(void*) ref_queue_weak,
		(void*) rql, (void*) rql->ref_queue);
#endif

	*rql_prev = rql->next;
	if ( ! unused && ref_queue != Qnil ) {
	  unused = rql;
	} else {
	  xfree(rql);
	}
	continue;
      }
      /* Check via weak object. */
      else if ( object == ref_queue ) 
	return ref_obj;

      /* Move to next. */
      rql_prev = &rql->next;
    }

    /* Use unused slot or create new a new one. */
    if ( ref_queue != Qnil ) {
      rql = unused ? unused : xmalloc(sizeof(*rql));
      rql->ref_queue = ref_queue_weak;
      rql->next = ref->ref_queues;
      ref->ref_queues = rql;

#if 0
      fprintf(stderr, "   rql: NEW  ref %p %lu for ref_queue %p in rql %p %p\n", 
	      (void*) ref, (unsigned long) ref->ref_id, 
	      (void*) ref_queue_weak,
	      (void*) rql, (void*) rql->ref_queue);
#endif
    }
  }
  
  return ref_obj;
}


/*
 * Document-class: Reference::WeakReference
 *
 * WeakReference class.
 *
 * Example:
 *
 *   require 'reference'
 *   GC.disable
 *   obj = Object.new
 *   ref = Reference::WeakReference.new(obj)
 *   ref.object.should == obj
 *   GC.enable
 *   ObjectSpace.garbage_collect
 *   ref.object.should == obj
 *   obj = nil
 *   ObjectSpace.garbage_collect
 *   ref.object.should == nil
 *   
 */

/* call-seq: 
 * WeakReference.new(object, ref_queue = nil)
 *
 * Returns a WeakReference for the object.
 * May return the same instance for a particular object and subclass.
 * Will return a new instance for immediate objects.
 * WeakReference#object will return the reference object, or nil if there are no other hard references to the object.
 * If ref_queue is not nil, the WeakReference will be added to ref_queue when object is not reachable.
 */
static VALUE
M_weak_new(int argc, VALUE *argv, VALUE cls)
{
  VALUE object, ref_queue, ref_obj;

  rb_scan_args(argc, argv, "11", &object, &ref_queue);
  ref_obj = ref_new(weak_table, cls, object, ref_queue, ref_mark, weak_free, 0);
#if REFERENCE_DEBUG > 2
  fprintf(stderr, "  WeakReference.new(%p, %p) => %p\n", (void*) cls, (void*) object, (void*) ref_obj);
#endif
  return ref_obj;
}

static VALUE
M_weak_each(VALUE cls)
{
  ref_table_foreach(weak_table, ref_yield, (st_data_t) 0);
  return cls;
}

static int 
ref_remove_dereferenced(st_data_t object, st_data_t ref_obj, st_data_t n_released_p)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));

  /* The referred object is no longer referenced elsewhere. */
  if ( ref->object == Qnil ) {
#if REFERENCE_DEBUG > 3
    fprintf(stderr, "      %p releasing\n", (void*) ref->object);
#endif
    return ST_DELETE;
  } 
#if REFERENCE_DEBUG > 3
  else {
    fprintf(stderr, "      %p live\n", (void*) ref->object);
  }
#endif

  return ST_CONTINUE;
}

static int 
ref_maybe_dereference(st_data_t object, st_data_t ref_obj, st_data_t n_released_p)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));

#if REFERENCE_DEBUG > 2
  fprintf(stderr, "    ref_maybe_dereference(object=%p, ref_obj=%p): ref %p\n", (void*) object, (void*) ref_obj, (void*) ref);
#endif

  /* The Reference itself is no longer referenced. */
  if ( ! RB_GC_MARKED(ref->ref_object) ) {
#if REFERENCE_DEBUG > 2
    fprintf(stderr, "       ref_maybe_dereference(object=%p, ref_obj=%p): ref_obj UNMARKED\n", (void*) object, (void*) ref_obj);
#endif
    ref->ref_table = 0;
    return ST_DELETE;
  }

  /* The referred object is no longer referenced elsewhere. */
  if ( ! RB_GC_MARKED(ref->object) ) {
#if REFERENCE_DEBUG > 3
    fprintf(stderr, "      %p releasing\n", (void*) ref->object);
#endif
    ++ * ((size_t*) n_released_p);
    return ref_dereference(ref);
  } 
#if REFERENCE_DEBUG > 3
  else {
    fprintf(stderr, "      %p live\n", (void*) ref->object);
  }
#endif

  return ST_CONTINUE;
}

static void
weak_remove_dead_references(void *callback, void *func_data)
{
  size_t n_released = 0;

#if REFERENCE_DEBUG > 1
  fprintf(stderr, "\n  weak_remove_dead_references()\n");
#endif

  ref_table_foreach(weak_table, ref_maybe_dereference, (st_data_t) &n_released);

#if REFERENCE_DEBUG > 0
  if ( n_released )
  fprintf(stderr, "  weak_remove_dead_references(): %lu released\n\n", 
	  (unsigned long) n_released,
	  );
#endif
}

/* call-seq:
 * WeakReference._mri_cached_instance_count.
 *
 * Returns the number of the WeakReferences (and subclasses) in the instance cache.
 */
static VALUE 
M_weak_cached_instance_count(VALUE cls)
{
  size_t count = 0;
  st_foreach(weak_table, ref_table_count, (st_data_t) &count);
  return SIZET2NUM(count);
}


/*
 * Document-class: Reference::SoftReference
 *
 * SoftReference class.
 *
 * SoftReference mantain a reference unless they are not traversed within a number of collections.
 *
 */

/* call-seq: 
 * SoftReference.new(object, ref_queue = nil)
 *
 * Returns a SoftReference for the object.
 * May return the same instance for a particular object for each subclass.
 * Will return a new instance for immediate objects.
 * In general, SoftReference maintains a hard reference to object if SoftReference#object has been traversed reciently or until some amount of memory pressure.  The actual behavior is implementation-specific.
 * If ref_queue is not nil, the SoftReference will be added to ref_queue when object is not reachable.
 */
static VALUE
M_soft_new(int argc, VALUE *argv, VALUE cls)
{
  VALUE object, ref_queue, ref_obj;

  rb_scan_args(argc, argv, "11", &object, &ref_queue);
  ref_obj = ref_new(soft_table, cls, object, ref_queue, soft_mark, soft_free, soft_gc_ttl);
#if SOFT_REFERENCE_DEBUG >= 1
  fprintf(stderr, "  SoftReference.new(%p, %p) => %p\n", (void*) cls, (void*) object, (void*) ref_obj);
#endif
  return ref_obj;
}

static VALUE
M_soft_each(VALUE cls)
{
  dereference_count = 0;
  ref_table_foreach(soft_table, ref_yield, (st_data_t) 0);
  if ( dereference_count ) {
    ref_table_foreach(soft_table, ref_remove_dereferenced, (st_data_t) 0);
    dereference_count = 0;
  }
  return cls;
}

static void
soft_remove_dead_references(void *callback, void *func_data)
{
  size_t n_released = 0;

#if SOFT_REFERENCE_DEBUG >= 2
  fprintf(stderr, "\n  soft_remove_dead_references()\n");
#endif

  ref_table_foreach(soft_table, ref_maybe_dereference, (st_data_t) &n_released);

#if SOFT_REFERENCE_DEBUG >= 1
  if ( n_released )
  fprintf(stderr, "  soft_remove_dead_references(): %lu released\n\n", 
	  (unsigned long) n_released
	  );
#endif
}

static int 
soft_maybe_dereference(st_data_t object, st_data_t ref_obj, st_data_t gc_left_decr_p)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  int gc_left_decr = * (int*) gc_left_decr_p;

  if ( ref->traversed_since_gc ) {
    ref->gc_left = ref->gc_ttl;
  } else {
    if ( ref->gc_left > gc_left_decr )
      ref->gc_left -= gc_left_decr;
    else
      ref->gc_left = 0;
  }

#if SOFT_REFERENCE_DEBUG >= 1
  fprintf(stderr, "    soft_maybe_dereference(object=%p, ref_obj=%p): ref %p gc_left=%d traversed_since_gc=%d : gc_left_decr %d\n", 
	  (void*) ref->object, (void*) ref->ref_object, (void*) ref, 
	  (int) ref->gc_left, (int) ref->traversed_since_gc, (int) gc_left_decr);
#endif

  ref->traversed_since_gc = 0;

  /* 
     The SoftReference has been untraversed for too many GCs or too much memory pressure was applied.
  */
  if ( ref->gc_left <= 0 ) {
#if SOFT_REFERENCE_DEBUG >= 2
    fprintf(stderr, "      %p discarded\n", (void*) ref->object);
#endif
    return ref_dereference(ref);
  }
#if SOFT_REFERENCE_DEBUG >= 3
  else {
   fprintf(stderr, "      %p live\n", (void*) ref->object);
  }
#endif

  return ST_CONTINUE;
}

static int soft_memory_pressure;

/* call-seq:
 * SoftReference._mri_memory_pressure
 *
 * Returns the number of times memory pressure occurred since last GC.
 */
static VALUE 
M_soft_memory_pressure(VALUE cls)
{
  return INT2FIX(soft_memory_pressure);
}

/* call-seq:
 * SoftReference._mri_memory_pressure= Fixnum
 *
 * Sets the number of times memory pressure occurred since last GC.
 * SoftReference._mri_memory_pressure is set to 0 after every GC.
 */
static VALUE 
M_soft_memory_pressure_set(VALUE cls, VALUE value)
{
  soft_memory_pressure = FIX2INT(value);
  return cls;
}

/* call-seq:
 * SoftReference._mri_memory_pressure_occurred.
 *
 * Called after each GC if memory_pressure occurred.
 */
static VALUE 
M_soft_memory_pressure_occurred(VALUE cls)
{
  return cls;
}

static void
soft_memory_pressure_occurred(void *callback, void *func_data)
{
#if SOFT_REFERENCE_DEBUG >= 1
  if ( ! soft_memory_pressure ) {
    fprintf(stderr, "  soft_memory_pressure()\n");
  }
#endif
  ++ soft_memory_pressure;
}

static unsigned int
soft_memory_pressure_adjust();

static VALUE
M_soft_decrement_gc_left(VALUE cls)
{
  int gc_left_decr = 1;

  if ( soft_memory_pressure ) {
#if SOFT_REFERENCE_DEBUG >= 1
    if ( ! soft_memory_pressure ) {
      fprintf(stderr, "  soft_decrement_gc_left(): soft_memory_pressure = %d\n", (int) soft_memory_pressure);
    }
#endif
    gc_left_decr += soft_memory_pressure_adjust();
  }

  ref_table_foreach(soft_table, soft_maybe_dereference, (st_data_t) &gc_left_decr);

  return cls;
}

static VALUE
M_soft_after_gc(VALUE cls)
{
  rb_funcall(cls, id__mri_decrement_gc_left, 0);
  return cls;
}

static void 
soft_after_gc(void *callback, void *func_data)
{
  if ( soft_memory_pressure ) {
#if SOFT_REFERENCE_DEBUG >= 1
    if ( ! soft_memory_pressure ) {
      fprintf(stderr, "  soft_decrement_gc_left(): soft_memory_pressure = %d\n", (int) soft_memory_pressure);
    }
#endif
    rb_funcall(rb_cSoftReference, id__mri_memory_pressure_occurred, 0);
  }
  rb_funcall(rb_cSoftReference, id__mri_after_gc, 0);
  soft_memory_pressure = 0;
}

/* SoftReference memory pressure. */

static int 
soft_gc_left_count(st_data_t object, st_data_t ref_obj, st_data_t count_p)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));

  ((size_t*) count_p)[0] += ref->gc_left;
  ((size_t*) count_p)[1] += 1;

  return ST_CONTINUE;
}


static unsigned int
soft_memory_pressure_adjust()
{
  unsigned int gc_left_decrement = 0;
  size_t count[2] = { 0, 0 };

#if SOFT_REFERENCE_DEBUG >= 2
  fprintf(stderr, "  soft_memory_pressure_adjust()\n");
#endif

  /* Compute the average SoftReference#gc_left and divide by some factor and multiply the soft_memory_pressure. */
  ref_table_foreach(soft_table, soft_gc_left_count, (st_data_t) &count);

  if ( count[1] > 0 ) {
    gc_left_decrement = (unsigned int) (count[0] / count[1]); /* average gc_left */
    gc_left_decrement /= 2; /* scale by half. */
    if ( gc_left_decrement == 0 ) gc_left_decrement ++; /* insure some effect. */
#if 0
    /* This might be too heavy handed. */
    gc_left_decrement *= soft_memory_pressure; /* scale by soft_memory_pressure. */
#endif
  }

#if SOFT_REFERENCE_DEBUG >= 2
  fprintf(stderr, "  soft_memory_pressure_adjust(): %lu references soft_memory_pressure=%d gc_left_decrement=%d\n", (unsigned long) count[1], (int) soft_memory_pressure, (int) gc_left_decrement);
#endif

  return gc_left_decrement;
}

/* call-seq:
 * SoftReference._mri_cached_instance_count
 *
 * Returns the number of SoftReferences (and subclasses) in the instance cache.
 */
static VALUE 
M_soft_cached_instance_count(VALUE cls)
{
  size_t count = 0;
  st_foreach(soft_table, ref_table_count, (st_data_t) &count);
  return SIZET2NUM(count);
}

/* call-seq:
 * SoftReference#object
 *
 *  Returns the referenced object or nil if the object is no longer reachable.
 *  Each call to #object may delay collection of object.
 *  A SoftReference may drop its #object upon memory pressure or after a number of collections since last #object call.
 */
static VALUE 
m_soft_object(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  if ( ref->object != Qnil )
    ref->traversed_since_gc = 1; /* can be expensive for REE. */
  return ref->object;
}

/* call-seq:
 * SoftReference#_mri_traversed_since_gc
 *
 *  Returns true if #object was called since last GC.
 */
static VALUE 
m_soft_traversed_since_gc(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  return ref->traversed_since_gc ? Qtrue : Qfalse;
}

/* call-seq:
 * SoftReference#_mri_traversed_since_gc=
 *
 */
static VALUE 
m_soft_traversed_since_gc_set(VALUE ref_obj, VALUE bool)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  ref->traversed_since_gc = (bool == Qnil || bool == Qfalse) ? 0 : 1;
  return ref_obj;
}

/* call-seq:
 * SoftReference._mri_gc_ttl
 *
 * Returns the default number of GCs until the unreferenced object may be dropped.
 */
static VALUE 
M_soft_gc_ttl(VALUE cls)
{
  return INT2FIX(soft_gc_ttl);
}

/* call-seq:
 * SoftReference._mri_gc_ttl=
 *
 * Sets the default number of GCs until the unreferenced object may be dropped.
 */
static VALUE 
M_soft_gc_ttl_set(VALUE cls, VALUE i)
{
  soft_gc_ttl = FIX2INT(i);
  return cls;
}

/* call-seq:
 * SoftReference#_mri_gc_ttl
 * 
 * Returns the number of GCs until the unreferenced object may be dropped.
 */
static VALUE 
m_soft_gc_ttl(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  return INT2FIX(ref->gc_ttl);
}

/* call-seq:
 * SoftReference#_mri_gc_ttl=
 * 
 * Sets the number of GCs until the unreferenced object may be dropped.
 */
static VALUE 
m_soft_gc_ttl_set(VALUE ref_obj, VALUE i)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  ref->gc_ttl = FIX2INT(i);
  return ref_obj;
}

/* call-seq:
 * SoftReference#_mri_gc_left
 *
 * Returns the number of GCs left until the unreferenced object may be dropped.
 * This number is set to #_mri_gc_ttl if #_mri_traversed_since_gc is true after GC.
 * When this count reaches zero, #object may be forced to be nil.
 */
static VALUE 
m_soft_gc_left(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  return INT2FIX(ref->gc_left);
}


/* call-seq:
 * SoftReference#_mri_gc_left=
 * 
 * Sets the number of GCs left until the unreferenced object may be dropped.
 */
static VALUE 
m_soft_gc_left_set(VALUE ref_obj, VALUE i)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));
  ref->gc_left = FIX2INT(i);
  return ref_obj;
}


/*********************************************************************
 * ReferenceQueue
 */

static VALUE
m_reference_notify_reference_queues(VALUE ref_obj)
{
  rb_reference *ref = RB_REFERENCE_CHECK(DATA_PTR(ref_obj));

  if ( ref->notify_ref_queue ) {
    rb_ref_queue_list *ref_queues = ref->ref_queues;

#if REFERENCE_QUEUE_DEBUG > 2
    fprintf(stderr, "    ref_notify_reference_queues(ref %p):\n", (void*) ref);
    fprintf(stderr, "      ref_obj = %p\n", (void*) ref->ref_object);
    fprintf(stderr, "      ref_queues = %p\n", (void*) ref_queues);
#endif

    /* Remove ReferenceQueue from Reference's ref_queues list. */
    while ( (ref_queues = ref->ref_queues) ) {
      VALUE ref_queue = ref_queues->ref_queue;
      ref_queues->ref_queue = Qnil;
      ref->ref_queues = ref_queues->next;
      xfree(ref_queues);
      
#if REFERENCE_QUEUE_DEBUG > 3
      fprintf(stderr, "        ref_queue = %p: weak => \n", (void*) ref_queue);
#endif
      /* Traverse Reference: ReferenceQueue may have been sweeped. */
      if ( ref_queue != Qnil && (ref_queue = rb_funcall(ref_queue, id_object, 0)) != Qnil ) {
#if REFERENCE_QUEUE_DEBUG > 3
	fprintf(stderr, "          ref_queue = %p: real\n", (void*) ref_queue);
#endif
	/* Push Reference onto Reference's ReferenceQueue. */
	rb_funcall2(ref_queue, id__mri_push_reference, 1, &ref->ref_object);
      }
    }
    
    /* No more ReferenceQueues left. */
    ref->notify_ref_queue = 0;
    assert(! ref->ref_queues);
  }

  return ref_obj;
}

static void 
notify_reference_queues(void *callback, void *func_data)
{
  rb_reference *ref;

  while ( (ref = queued_refs) ) {
    rb_funcall(ref->ref_object, id__mri_notify_reference_queues, 0);
    assert(queued_refs_count);
    -- queued_refs_count;
    queued_refs = ref->next;
  }

  ++ gc_count;
}

void 
Init_reference(void)
{
  const char *str;

  ref_trace = (str = getenv("RUBY_REF_TRACE")) && atoi(str) > 0;

  /* Implementation-inspecific selectors. */
  id_object = rb_intern("object");
  id_object_id = rb_intern("object_id");
  /* Implementation-specific selectors. */
  id__mri_notify_reference_queues = rb_intern("_mri_notify_reference_queues");
  id__mri_push_reference = rb_intern("_mri_push_reference");
  id__mri_memory_pressure_occurred = rb_intern("_mri_memory_pressure_occurred");
  id__mri_decrement_gc_left = rb_intern("_mri_decrement_gc_left");
  id__mri_after_gc = rb_intern("_mri_after_gc");

  weak_table = st_init_table(&ref_table_type);
  soft_table = st_init_table(&ref_table_type);

  rb_mReference = rb_define_module("Reference");

  rb_cReference = rb_define_class_under(rb_mReference, "Reference", rb_cObject);
  rb_define_method(CLASS_OF(rb_cReference), "_mri_live_instance_count", M_reference_live_instance_count, 0);
  rb_define_method(rb_cReference, "object", m_reference_object, 0);
  rb_define_method(rb_cReference, "referenced_object_id", m_reference_referenced_object_id, 0);
  rb_define_method(rb_cReference, "_mri_object", m_reference_object, 0);
  rb_define_method(rb_cReference, "_mri_object=", m_reference_object_set, 1);
  rb_define_method(rb_cReference, "_mri_dereference", m_reference_dereference, 0);
  rb_define_method(rb_cReference, "_mri_reference_id", m_reference_reference_id, 0);
  rb_define_method(rb_cReference, "_mri_add_reference_queue", m_reference_add_reference_queue, 1);
  rb_define_method(rb_cReference, "_mri_notify_reference_queues", m_reference_notify_reference_queues, 0);

  rb_cWeakReference = rb_define_class_under(rb_mReference, "WeakReference", rb_cReference);
  rb_define_method(CLASS_OF(rb_cWeakReference), "new", M_weak_new, -1);
  rb_define_method(CLASS_OF(rb_cWeakReference), "_mri_each", M_weak_each, 0);
  rb_define_method(CLASS_OF(rb_cWeakReference), "_mri_cached_instance_count", M_weak_cached_instance_count, 0);

  rb_cSoftReference = rb_define_class_under(rb_mReference, "SoftReference", rb_cReference);
  rb_define_method(CLASS_OF(rb_cSoftReference), "new", M_soft_new, -1);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_each", M_soft_each, 0);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_memory_pressure", M_soft_memory_pressure, 0);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_memory_pressure=", M_soft_memory_pressure_set, 1);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_memory_pressure_occurred", M_soft_memory_pressure_occurred, 0);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_after_gc", M_soft_after_gc, 0);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_decrement_gc_left", M_soft_decrement_gc_left, 0);
  
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_cached_instance_count", M_soft_cached_instance_count, 0);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_gc_ttl", M_soft_gc_ttl, 0);
  rb_define_method(CLASS_OF(rb_cSoftReference), "_mri_gc_ttl=", M_soft_gc_ttl_set, 1);
  rb_define_method(rb_cSoftReference, "object", m_soft_object, 0);
  rb_define_method(rb_cSoftReference, "_mri_traversed_since_gc", m_soft_traversed_since_gc, 0);
  rb_define_method(rb_cSoftReference, "_mri_traversed_since_gc=", m_soft_traversed_since_gc, 1);
  rb_define_method(rb_cSoftReference, "_mri_gc_ttl", m_soft_gc_ttl, 0);
  rb_define_method(rb_cSoftReference, "_mri_gc_ttl=", m_soft_gc_ttl_set, 1);
  rb_define_method(rb_cSoftReference, "_mri_gc_left", m_soft_gc_left, 0);
  rb_define_method(rb_cSoftReference, "_mri_gc_left=", m_soft_gc_left_set, 1);

  rb_gc_add_callback(RB_GC_PHASE_ALLOC, RB_GC_PHASE_BEFORE, soft_memory_pressure_occurred, 0);

  rb_gc_add_callback(RB_GC_PHASE_SWEEP, RB_GC_PHASE_BEFORE, remove_dead_reference_classes, 0);
  rb_gc_add_callback(RB_GC_PHASE_SWEEP, RB_GC_PHASE_BEFORE, weak_remove_dead_references, 0);
  rb_gc_add_callback(RB_GC_PHASE_SWEEP, RB_GC_PHASE_BEFORE, soft_remove_dead_references, 0);

  rb_gc_add_callback(RB_GC_PHASE_END,   RB_GC_PHASE_AFTER,  soft_after_gc, 0);
  rb_gc_add_callback(RB_GC_PHASE_END,   RB_GC_PHASE_AFTER,  notify_reference_queues, 0);

#if REFERERENCE_DEBUG > 0
  fprintf(stderr, "  Init_reference(void): READY\n");
#endif
}

