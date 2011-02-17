$:.unshift(File.dirname(__FILE__))
require 'reference_test'

class WeakReferenceTest < ReferenceTest
  def setup
    # srand(15491)
  end

  def reference
    @reference ||= WeakReference
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
    when_mri { assert_equal n_objects, cached_instance_count }
    Proc.new {
    n.each do | i |
      assert o[i]
      assert r[i].object
      assert_equal o[i], r[i].object
      assert_equal o[i].object_id, r[i].referenced_object_id
      nil
    end
    }.call
    _test_free_after_gc
  end

  def test_free_ref_after_gc
    prepare!
    when_mri { assert_equal n_objects, cached_instance_count }
    run_gc!(:clear => [ :o, :r ])
    when_mri { assert_equal 0, cached_instance_count }
  end

  def test_ref_immediate
    prepare! { | i | i }
    when_mri { assert_equal 0, cached_instance_count } # FIXME
    when_mri { assert cached_instance_count <= 1 }
    run_gc!(:clear => :o)
    n.each do | i |
      when_mri { $stderr.puts "ref_id: #{r[i]._mri_reference_id}" } if @verbose
      assert_equal i, r[i].object
      assert_equal i.object_id, r[i].referenced_object_id
      nil
    end
    nil
  end

  class MyReference < WeakReference
    
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
