#!/bin/sh
set -ex
script_dir="`dirname $0`"
base_dir="`cd $script_dir/../../ && /bin/pwd`"
FILE="${FILE:-*_test.rb}"
error=0
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

exit $error
