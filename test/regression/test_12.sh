#!/bin/bash

PISM_PATH=$1
MPIEXEC=$2

# Test name:
echo "Test #12: mass conservation within rounding error (SIA moving margin)."
# The list of files to delete when done.
files="verify.nc ivol.nc"

rm -f $files
GRID="-Mx 31 -My 31 -Mz 31 -y 5000 -ys 1000"
OPTS="-test B -max_dt 25 -o_size small"
TS_OPTS="-ts_file ivol.nc -ts_vars ivol -ts_times 1000:25:1e4"
# run test B 
$MPIEXEC -n 2 $PISM_PATH/pismv -test B $GRID $OPTS $TS_OPTS

/usr/bin/env python <<EOF
from numpy import diff, log10, floor
from sys import exit
try:
    from netCDF3 import Dataset
except:
    from netCDF4 import Dataset

nc = Dataset("ivol.nc", 'r')
ivol = nc.variables['ivol'][:]
ivol_max = ivol.max()
threshold = 10**(floor(log10(ivol_max)) - 12) # 12 digits of accuracy
diff_max = diff(ivol).max()

if diff_max < threshold:
    exit(0)
else:
    print "diff(ivol).max() = %f > %f" % (diff_max, threshold)
    exit(1)
EOF

if [ $? != 0 ];
then
    exit 1
fi

rm -f $files; exit 0

