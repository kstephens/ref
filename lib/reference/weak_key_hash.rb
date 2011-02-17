require 'reference'
require 'reference/safe_monitor'

module Reference
  # A hacked up, proof-of-concept, prototype weak key Hash.
  # Cannot use nil as key.
  # Keys are assumed to be non-immediate objects.
  # Not thread-safe.
  class WeakKeyHash < Hash
    def __ref_queue
      @__ref_queue ||= ReferenceQueue.new
    end
    def __ref_queue_poll
      if ref = __ref_queue.pop
        __delete__(ref)
        true
      else
        false
      end
    end
    def __ref_queue_poll_all
      while __ref_queue_poll
        true
      end
    end


    alias :__delete__ :delete
    def delete key
      __ref_queue_poll_all
      if key != nil
        key = WeakReference.new(key, __ref_queue)
        __delete__(key)
      end
    end

    alias :__keys__ :keys
    def keys
      __ref_queue_poll_all
      __keys__.map{|key| key.object}.compact
    end

    alias :__size__ :size
    def size
      keys.size
    end

    alias :__get__ :[]
    # Ignored if key == nil.
    def [] key
      __ref_queue_poll_all
      if key != nil
        key = WeakReference.new(key)
        __get__(key)
      else
        nil
      end
    end

    alias :__set__ :[]=
    # Ignored if key == nil.
    def []= key, value
      __ref_queue_poll_all
      if key != nil
        key = WeakReference.new(key, __ref_queue)
        __set__(key, value)
      end
    end

    def each 
      keys.each do | key |
        yield key, self[key] 
      end
    end
  end
end
