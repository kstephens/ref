$:.unshift(File.dirname(__FILE__))
require 'reference_test'

class MriReferenceQueueTest < ReferenceTest
  def setup
    # srand(15491)
  end

  def reference
    @reference ||= WeakReference
  end

  def fixme
    yield
  end

  def make_ref_queue
    ref_queue = [ ] 
    def ref_queue._mri_push_reference(ref)
      $stderr.puts "  Queued #{ref} #{ref._mri_reference_id}" if @verbose
      self << ref
    end
    ref_queue
  end

  def test_reference_queue
    return unless when_mri
    prepare!
    fixme { assert_equal n_objects, cached_instance_count }
    # assert_equal n_objects, cached_instance_count - 1

    ref_queue = make_ref_queue

    # Add ref_queue to each reference.
    r.each do | ref |
      ref._mri_add_reference_queue(ref_queue)
    end
    run_gc!
    fixme { assert_equal n_objects + 1, cached_instance_count }
    #assert_equal n_objects + 1, cached_instance_count + 1

    # Add same ref_queue multiple times.
    r.each do | ref |
      ref._mri_add_reference_queue(ref_queue)
    end
    run_gc!
    fixme { assert_equal n_objects + 1, cached_instance_count } 
    # assert_equal n_objects + 1, cached_instance_count + 1

    # Force GC of objects.
    run_gc!(:clear => :o)
    # Expect each Reference to be in ref_queue.
    n.each do | i |
      ref = r[i]
      unless ref_queue.include?(ref)
        puts "ref #{i} #{ref} #{ref._mri_reference_id} was not queued"
      end
    end
    n.each do | i |
      ref = r[i]
      assert ref_queue.include?(ref), "ref #{i} #{ref} #{ref._mri_reference_id} was not queued"
    end
    assert_equal n_objects, ref_queue.size
    
    # Expecte ref_queue and references to be dequeued.
    ref_queue = nil
    run_gc!(:clear => :r)
    fixme { assert_equal 0, cached_instance_count }
    # assert cached_instance_count <= 1
  end

  def test_many_reference_queues
    return unless when_mri
    prepare!
    assert_equal n_objects, cached_instance_count

    ref_queues = (0..10).map { | i | make_ref_queue }
    r.each do | ref |
      ref_queues.each do | ref_queue |
        ref._mri_add_reference_queue(ref_queue)
      end
    end
    r.each do | ref |
      ref_queues.each do | ref_queue |
        ref._mri_add_reference_queue(ref_queue)
      end
    end
    run_gc!
    assert_equal n_objects + ref_queues.size, cached_instance_count

    run_gc!(:clear => :o)

    # Cleanup reference queues.
    ref_queues = nil
    run_gc!
    r.each do | ref |
      ref._mri_add_reference_queue(nil)
    end

    run_gc!
    fixme { assert_equal 0, cached_instance_count }
    # assert cached_instance_count <= 1

    run_gc!(:clear => :r)
    run_gc!
    fixme { assert_equal 0, cached_instance_count }
    # assert cached_instance_count <= 1
  end

  def test_weak_reference_queue
    return unless when_mri
    prepare!
    assert_equal n_objects, cached_instance_count

    ref_queue = make_ref_queue

    # Add ref_queue to each reference.
    r.each do | ref |
      ref._mri_add_reference_queue(ref_queue)
    end
    run_gc!
    assert_equal n_objects + 1, cached_instance_count

    # Remove hard reference to ref_queue,
    # and cleanup weak reference queues.
    ref_queue = nil
    run_gc!
    r.each do | ref |
      ref._mri_add_reference_queue(ref_queue)
    end
    run_gc!
    assert_equal n_objects, cached_instance_count

    run_gc!(:clear => [ :o, :r ])
    assert_equal 0, cached_instance_count
  end
end
