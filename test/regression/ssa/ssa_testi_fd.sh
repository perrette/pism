#!/bin/bash

# SSAFD verification test I regression test

PISM_PATH=$1
MPIEXEC=$2
PISM_SOURCE_DIR=$3

# List of files to remove when done:
files="foo.nc foo.nc~ test-I-out.txt"

rm -f $files

set -e

OPTS="-verbose 1 -ssa_method fd -o foo.nc -ssa_rtol 5e-07 -ksp_rtol 1e-12 -Mx 5"

# do stuff
$PISM_PATH/ssa_testi -My 61 $OPTS > test-I-out.txt
$PISM_PATH/ssa_testi -My 121 $OPTS >> test-I-out.txt

set +e

# Check results:
diff test-I-out.txt -  <<END-OF-OUTPUT
NUMERICAL ERRORS in velocity relative to exact solution:
velocity  :  maxvector   prcntavvec      maxu      maxv       avu       avv
                4.7417      0.05219    4.7417    0.1976    0.4041    0.0087
NUM ERRORS DONE
NUMERICAL ERRORS in velocity relative to exact solution:
velocity  :  maxvector   prcntavvec      maxu      maxv       avu       avv
                1.3907      0.01351    1.3907    0.0385    0.1050    0.0018
NUM ERRORS DONE
END-OF-OUTPUT

if [ $? != 0 ];
then
    exit 1
fi

rm -f $files; exit 0
