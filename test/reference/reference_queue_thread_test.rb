$:.unshift(File.dirname(__FILE__))
require 'reference_test'
require 'reference/reference_queue'


class ReferenceQueueThreadTest < ReferenceTest
  def setup
    super
    @objs = [ ]
    @refs = [ ]
    @ref_queue = ReferenceQueue.new
  end

  def ref_queue_thread ref_cls
    refs_in = refs_out = 0
    @ref_queue_thread = Thread.new do
      sleep 2
      until @refs.empty?
        if ref = @ref_queue.pop
          refs_out += 1
          # $stderr.puts "Worker: popped #{ref}"
          $stderr.write '-'
          @refs.delete(ref)
        end
        sleep(0.02 + rand(0.01))
        gc!
      end
    end

    50.times do | i |
      @objs << (obj = "#{i}")
      @refs << (ref = ref_cls.new(obj, @ref_queue))
      refs_in += 1
      # $stderr.puts "Main: created obj #{obj.inspect} ref #{ref}"
      $stderr.write '+'
      sleep(0.01 + rand(0.01))
      @objs.delete(@objs[rand(@objs.size)])
      gc!
    end
    @objs.clear

    @ref_queue_thread.join

    assert_equal 0, @objs.size
    assert_equal 0, @refs.size
    assert_equal refs_in, refs_out

    $stderr.puts " OK"
  end

  def gc!
    if rand(3) == 0
      $stderr.write 'G'
      GC.start
    end
  end

  def test_weak_ref_thread
    ref_queue_thread WeakReference
  end

  def test_soft_ref_thread
    ref_queue_thread SoftReference
  end
end

