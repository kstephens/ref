require 'reference'

module Reference
  # Pseudo-code for MRI SoftReference heuristics:
  #
  # * Each SoftReference#_mri_gc_left and SoftRefernce#_mri_gc_ttl are set to some arbitrary value: SoftReference._mri_gc_ttl (default = 10).
  # * If SoftReference#object was traversed since last GC, SoftReference#_mri_gc_left is reset to SoftRereference#_mri_gc_ttl.
  # * If SoftReference#object was not traversed since last GC, SoftReference#_mri_gc_left is decremented by 1.
  # * If memory pressure occurred, SoftReference#_mri_gc_left is decremented by half of the average of all SoftReference#_mri_gc_left.
  # * When SoftReference#_mri_gc_left reaches 0, SoftReference#object= nil.
  # * "Memory pressure" is whenever MRI allocates a new GC heap.
  class SoftReference
=begin
    # Code built into reference.c.
    attr_accessor :_mri_traversed_since_gc, :_mri_gc_left, :_mri_gc_ttl

    def initialize object, ref_queue = nil
      @_mri_gc_left = @_mri_gc_ttl = self.class._mri_gc_ttl # default to 10
      @_mri_traversed_since_gc = false
      _mri_add_reference_queue(ref_queue) unless ref_queue == nil
    end

    def object
      self._mri_traversed_since_gc = true if @object != nil
      @object
    end

    (class << self; self; end).instance_eval do
      attr_accessor :_mri_memory_pressure
    end
    self._mri_memory_pressure = 0

    # Yield to block for each live SoftReference instance.
    def self._mri_each &block 
      # ...
    end
    
    # Called when MRI GC allocates a new heap.
    def self._mri_memory_pressure_occurred
      self._mri_memory_pressure += 1
    end

    # Clears #object, schedules notification of ReferenceQueues.
    def _mri_dereference
      ...
    end
=end

    def self._mri_gc_left_avg
      gc_left_sum = count = 0
      _mri_each do | ref |
        gc_left_sum += ref._mri_gc_left
        count += 1
        # ref = nil
      end
      count = 1 if count <= 0
      gc_left_sum /= count
      # $stderr.puts "  gc_left avg = #{gc_left_sum}"
      gc_left_sum
    end

    def self._mri_after_gc
      # $stderr.puts "#{self}._mri_after_gc"
      gc_left_decr = 1
      gc_left_avg = _mri_gc_left_avg # nil

      # If memory pressure occurred,
      #   Increase gc_left_decr by avg(#_mri_gc_left) / 2 * memory_pressure.
      if _mri_memory_pressure > 0
        # $stderr.puts "  memory_pressure = #{_mri_memory_pressure}"
        gc_left_avg ||= _mri_gc_left_avg
        gc_left_decr += gc_left_avg / 2
=begin
        # This might be too heavy handed.
        gc_left_decr *= _mri_memory_pressure 
=end
        self._mri_memory_pressure = 0
      end

      # $stderr.puts "  gc_left_decr = #{gc_left_decr} "

      _mri_each do | ref |
        # If SoftReference#object was traversed, reset gc_left.
        if ref._mri_traversed_since_gc
          ref._mri_gc_left = ref._mri_gc_ttl
        # If unreferenced for too many GC or enough memory pressure occurred, drop #object reference.
        elsif ref._mri_gc_left > gc_left_decr
          ref._mri_gc_left -= gc_left_decr
        else
          # $stderr.write "-"
          ref._mri_dereference
        end
        # Reset traversed_since_gc.
        ref._mri_traversed_since_gc = false
        # ref = nil
      end
      # $stderr.puts ""
    end
  end
end

