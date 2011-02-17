require 'test/unit'
require 'reference'

class ReferenceTest < Test::Unit::TestCase
  include Reference
  attr_accessor :n, :o, :oid, :r
  attr_accessor :gc_n
  attr_accessor :n_objects
  attr_accessor :reference
  attr_accessor :ruby_impl

  def ruby_impl
    @ruby_impl ||=
      case RUBY_DESCRIPTION
      when /^jruby/
        :jruby
      when /^rbx/
        :rbx
      else
        :mri
      end
  end
  def when_jruby
    ruby_impl == :jruby ? (block_given? ? yield : true) : nil
  end
  def when_mri
    ruby_impl == :mri ? (block_given? ? yield : true) : nil 
  end
  def when_rbx
    ruby_impl == :rbx ? (block_given? ? yield : true) : nil 
  end

  def setup
    GC.stress = (ENV['RUBY_GC_STRESS'] || '0').to_i > 0
  end

  def teardown
    clear! :ALL
    # ObjectSpace.garbage_collect
  end

  def n_objects
    @n_objects ||= (ENV['N_OBJECTS'] || '1000').to_i
  end

  def gc_n
    @gc_n ||= 2
  end

  def cached_instance_count
    when_mri { reference._mri_cached_instance_count }
  end

  def clear! clear = nil
    clear ||= [ ]
    clear = [ :n, :o, :oid, :r ] if clear == :ALL
    clear = [ clear ] unless Array === clear
    clear.each do | var |
      a = send(var);
      a.clear if a && a.respond_to?(:clear)
      send(:"#{var}=", nil)
      nil
    end
    nil
  end

  def run_gc! opts = { }
    gc_n = opts[:gc_n] || self.gc_n
    clear = opts[:clear] || [ ]
    # Force stack to clear.
    proc = Proc.new { 10.times { Array.new(1); nil }; nil }
    clear! clear
    (gc_n - 1).times do 
      Proc.new {
        10.times(&proc)
        yield if block_given?
        ObjectSpace.garbage_collect
        10.times(&proc)
        nil
      }.call
      nil
    end
    ObjectSpace.garbage_collect
    if @verbose 
      when_mri { puts "Reference._mri_live_instance_count = #{Reference._mri_live_instance_count}" }
    end
    nil
  end

  def prepare! &blk
    # $stderr.puts "\n#{caller.first}"
    run_gc!(:clear => :ALL)
    blk ||= lambda { | i | "#{reference}: %d" % i }
    Proc.new { 
      @n = (0 ... n_objects).to_a
      @o = @n.map(&blk)
      @oid = @o.map{|o| o.object_id }
      @r = @n.map{|i| reference && reference.new(@o[i]) }
      nil
    }.call
    if reference
      when_mri do
        unless Integer === @o[0]
          n_ref = 0
          reference._mri_each do | ref |
            assert ref
            n_ref += 1
          end
          $stderr.puts "n_ref = #{n_ref} @r.size = #{@r.size}\n" if @verbose
          assert n_ref >= @r.size
        end
      end
      nil
    end
    nil
  end

  def _test_free_after_gc
    run_gc!(:clear => [ :o, :oid ])
    n.each do | i |
      when_mri { $stderr.puts "r[#{i}] => #{r[i]._mri_reference_id}" } if @verbose
      assert r[i]
      assert_equal nil, r[i].object
      assert_equal nil, r[i].referenced_object_id
      nil
    end
    nil
  end

  def test_nothing
    assert true
  end
end
