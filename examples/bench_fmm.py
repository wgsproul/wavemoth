#!/usr/bin/env python

from __future__ import division

# Stick .. in PYTHONPATH
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname('__file__'), '..'))

import numpy as np
from wavemoth.fmm import *
from wavemoth.benchmark_utils import *

NQUAD = 28
Nside = 512
mmax = 2 * Nside
Nrings = 2 * Nside

nx = mmax // 2
ny = Nrings

q = np.sin(np.arange(nx) / 2)
x_grid = np.linspace(0, 1, nx)
y_grid = np.linspace(.01, 1 -.01, ny)

J = 10

both_grid = np.hstack([x_grid, y_grid])
out = both_grid * 0
with benchmark('libc_exp', J, profile=False):
    bench_libc_exp(both_grid, out, 2 * NQUAD * J * (mmax // 2))
assert np.all(out == np.exp(both_grid))

with benchmark('vml_exp', J, profile=False):
    bench_vml_exp(both_grid, out, 2 * J  * (mmax // 2))


#with benchmark('inv', J, profile=False):
#    bench_inv(both_grid, out, 8 * NQUAD * J)

with benchmark('mul', J, profile=False):
    bench_mul(both_grid, out, 2 * NQUAD * J  * (mmax // 2))

#out = y_grid * 0
#with benchmark('fmm', J, profile=False):
#    fmm1d(x_grid, q, y_grid, out, repeat=J)
