$:.unshift(File.dirname(__FILE__))
require 'reference_test'
require 'reference/reference_queue'


class ReferenceQueueTest < ReferenceTest

  class MyRef < WeakReference
    def cleanup
      puts "cleanup #{self}"
    end
  end

  def test_ref_queue
    GC.disable
    queue = ReferenceQueue.new
    ref = MyRef.new(Object.new, queue)
    assert_equal nil, queue.shift
    GC.enable
    run_gc!
    r = queue.shift
    assert_not_equal nil, r
    r.cleanup
  end
end

