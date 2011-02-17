$:.unshift(File.dirname(__FILE__))
require 'soft_reference_test'

class MriSoftReferenceTest < SoftReferenceTest
  def setup
    require 'reference/mri/soft_reference'
  end
end

