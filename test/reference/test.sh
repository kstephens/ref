#!/bin/sh
is_ree=
es=true
set -ex
if ruby -v | grep -s MBARI
then
  es='GC.copy_on_write_friendly=true GC.copy_on_write_friendly=false'
  is_ree=1
fi
script_dir="`dirname $0`"
base_dir="`cd $script_dir/../../ && /bin/pwd`"
FILE="${FILE:-*_test.rb}"
error=0
for e in $es
do
  export REF_TEST_PRE_EVAL="$e"
for f in $script_dir/$FILE
do
    cmd="ruby -I$base_dir/ext -I$base_dir/lib $f --verbose"
    if ! eval $DO_NOT_RUN $cmd 
    then
      error=1
      [ -n "$RUN_GDB" ] && gdb --args $cmd
      [ -n "$RUN_VALGRIND" ] && valgrind $cmd
    fi
done
done

exit $error
