require 'reference'
require 'reference/safe_monitor'

module Reference
  # Based on https://github.com/bdurand/ref by Brian Durand
  #
  # This class provides a simple thread safe container to hold a reference queue. Instances
  # of WeakReference and SoftReference can be added to the queue and as the objects pointed 
  # to by those references are cleaned up by the garbage collector, the references will be 
  # added to the queue.
  #
  # The reason for using a reference queue is that it tends to be more efficient than adding
  # individual finalizers to objects and the cleanup code can be handled by a thread outside
  # of garbage collection.
  #
  # In general, you should create your own subclass of WeakReference that contains the logic
  # needed to complete the cleanup. The object pointed to will have already been cleaned up
  # and the reference cannot maintain a reference to the object.
  #
  # === Example usage:
  #
  #   class MyRef < Ref::WeakReference
  #     def cleanup
  #       # Do something...
  #     end
  #   end
  #   
  #   queue = Ref::ReferenceQueue.new
  #   ref = MyRef.new(Object.new)
  #   queue.monitor(ref)
  #   queue.shift                 # = nil
  #   ObjectSpace.garbage_collect
  #   r = queue.shift             # = ref
  #   r.cleanup
  class ReferenceQueue
    def initialize
      @queue = []
      @lock = SafeMonitor.new
    end

=begin
# CANNOT BE IMPLEMENTED IN JRuby.
    # Monitor a reference. When the object the reference points to is garbage collected,
    # the reference will be added to the queue.
    def monitor(reference)
      @lock.synchronize do
        # Implementation-specific:
        reference._mri_add_reference_queue(self)
      end
    end
=end
    
    # Add a reference to the queue.
    # Called by Reference internally
    # Implementation-specific method.
    def _mri_push_reference(reference)
      if reference
        @lock.synchronize do
          @queue.push(reference)
        end
      end
    end
    
    # Pull the last reference off the queue. Returns +nil+ if their are no references.
    def pop
      @lock.synchronize do
        @queue.pop
      end
    end
    
    # Pull the next reference off the queue. Returns +nil+ if there are no references.
    def shift
      @lock.synchronize do
        @queue.shift
      end
    end
    
    # Return +true+ if the queue is empty.
    def empty?
      @queue.empty?
    end
    
=begin
# CANNOT BE IMPLEMENTED IN JRuby.
    # Get the current size of the queue.
    def size
      @queue.size
    end
=end
  end
end
