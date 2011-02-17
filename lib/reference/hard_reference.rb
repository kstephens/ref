require 'reference'

module Reference
  class HardReference < Reference
    def initialize object, ref_queue = nil
      @object = object
      # FIXME: How should HardReference behave with a ReferenceQueue?
    end
    def object
      @object
    end
    def referenced_object_id
      @object.object_id
    end
  end
end
