$:.unshift(File.dirname(__FILE__))
require 'reference_test'

class SoftReferenceTest < ReferenceTest
  def gc_n
    @gc_n ||= when_mri { SoftReference._mri_gc_ttl * 3 / 2 } || 10;
  end
  
  def reference
    @reference ||= SoftReference
  end

  def test_free_after_gc
    prepare!
    when_mri { assert_equal n_objects, cached_instance_count }
    _test_free_after_gc
    when_mri { assert_equal 0, cached_instance_count }
  end

  def test_keep_after_gc
    prepare!
    when_mri { assert_equal n_objects, cached_instance_count }
    run_gc!
    when_mri { assert_equal 0, cached_instance_count }
    n.each do | i |
      assert o[i]
      assert_equal nil, r[i].object
      assert_equal nil, r[i].referenced_object_id
    end
    _test_free_after_gc
  end

  def test_keep_after_one_gc
    prepare!
    when_mri { assert_equal n_objects, cached_instance_count }
    # Remove Hard References.
    run_gc!(:gc_n => 1, :clear => :o)
    r.each do | ref |
      assert ref.object
      when_mri { assert_equal ref._mri_gc_ttl - 1, ref._mri_gc_left }
    end
    when_mri { assert_equal n_objects, cached_instance_count }
  end

  def test_free_ref_after_gc
    prepare!
    when_mri { assert_equal n_objects, cached_instance_count } 
    run_gc!(:clear => [ :o, :r ])
    when_mri { assert_equal 0, cached_instance_count }
  end

  def test_ref_immediate
    prepare! { | i | i }
    when_mri { assert_equal 0, cached_instance_count }
    run_gc!(:clear => :o)
    n.each do | i |
      assert_equal i, r[i].object
      assert_equal i.object_id, r[i].referenced_object_id
    end
    nil
  end

  def test_ref_memory_pressure
    # return false # FIXME
    prepare!
    #run_gc!
    when_mri {
      assert cached_instance_count > 0
      assert cached_instance_count <= n_objects
    }

    # Force memory pressure (in MRI: this should cause new heaps).
    x = [ ]
    10000.times do | i |
      x << [ ]
      1000.times do | j |
        x[-1] << j.to_s
      end
    end
    run_gc!(:gc_n => 1)

    # Ensure that SoftReferences were dereferenced.
    n.each do | i |
      ref = r[i]
      # puts "i #{i} #{ref} => #{ref.object.inspect}"
      assert o[i]
      assert_equal nil, ref.object
      assert_equal nil, ref.referenced_object_id
    end
    when_mri { assert_equal 0, cached_instance_count }
  end

  class MyReference < SoftReference
    
  end

  def test_ref_subclass
    obj = Object.new
    ref1 = reference.new(obj)
    ref2 = MyReference.new(obj)
    assert reference === ref1
    assert reference === ref2
    assert ! (MyReference === ref1)
    assert MyReference === ref2
    assert_equal reference, ref1.class
    assert_equal MyReference, ref2.class
    assert_not_equal ref1.object_id, ref2.object_id
    assert_equal ref1.object, ref2.object
    assert_equal ref1.referenced_object_id, ref2.referenced_object_id
  end
end
