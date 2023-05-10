# typed: true

extend T::Sig

sig {params(blk: T.proc.params(xs: T::Array[Integer]).returns(Integer)).void}
def example(&blk)
  yield [0]
end

example(&:first)

example {|x| x.first}
