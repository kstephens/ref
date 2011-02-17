$:.unshift(File.dirname(__FILE__))
require 'reference_test'
require 'reference/weak_key_hash'
require 'reference/reference_queue'


class WeakKeyHashTest < ReferenceTest
  def test_weak_key_hash
    prepare!
    run_gc!(:clear => :r)

    hash = WeakKeyHash.new
    n.each do | i |
      hash[o[i]] = i
    end
    n.each do | i |
      assert_equal i, hash[o[i]]
    end

    run_gc!
    n.each do | i |
      assert_equal i, hash[o[i]]
    end

    n2 = (n.size / 2)
    n2.times do
      o.pop
      n.pop
      # r.pop
      oid.pop
    end
    n2 = o.size
    run_gc!

    assert_equal n2, hash.size
    assert_equal n2, hash.keys.size
    assert_equal n2, hash.values.size
    assert ! hash.keys.include?(nil)
  end
end

