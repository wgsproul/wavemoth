#!/usr/bin/env python
from __future__ import division

import os
import sys
import numpy as np
from spherew.butterflylib import *
from spherew.benchmark_utils import *
from spherew.fastsht import *

def post_projection_scatter():
    nvecs = 2
    N = 200
    a = np.zeros((N, nvecs))
    b = np.zeros((N, nvecs))
    mask = np.hstack([np.ones(N), np.zeros(N)])
    np.random.shuffle(mask)
    mask = mask.astype(np.int8)

    target1 = np.zeros((N + 20, nvecs))
    target2 = np.zeros((N - 20, nvecs))

    J = 5000000
    scatter(mask, target1, target2, a, add=True, not_mask=True, repeat=1)
    with benchmark('post_projection_scatter', J, profile=True):
        X = scatter(mask, target1, target2, a, add=True, not_mask=True, repeat=J)

def legendre_transform(nvecs):
    nx = 512
    nx -= nx % 6
    nk = 512
    x_squared = np.zeros(nx)
    a = np.zeros((nk, nvecs))
    y = np.zeros((nx, nvecs))
    p0 = np.zeros(nx)
    p1 = np.zeros(nx)

    def legendre_transform_normal(repeat):
        associated_legendre_transform(0, 0, a, y, x_squared, p0, p1,
                                      repeat=repeat)

    if nvecs == 2:
        benchmark(legendre_transform_normal, 1)
            
    J = 1000
    def legendre_transform_sse(repeat):
        associated_legendre_transform(0, 0, a, y, x_squared, p0, p1,
                                      repeat=repeat, use_sse=True)
    dt = benchmark(legendre_transform_sse, J, profile=False, duration=7)
    
    flops = nx * nk * (5 + 2 * nvecs)
    print 'GFLOP/sec:', flops / 1e9 / dt

def legendre_precompute():
    nvecs = 2
    nx = 2 * 2048 * 10
    nx -= nx % 6
    nk = 2 * 2048 // 2

#    nvecs = nk = nx = 1000

    m = nvecs
    n = nk
    k = nx

    A = np.ones((m, n), order='F')
    B = np.ones((k, n), order='C')
    1/0
    C = np.ones((10, 10), order='F')
    from spherew.blas import dgemm_crc
    J = 10
    dgemm_crc(A, P, C, repeat=1)
    with benchmark('dgemm', J):
        dgemm_crc(A, P, C, repeat=J, beta=1)
    flops = nvecs * nx * nk * 2
    print 'Number of GFLOPS performed', flops / 1e9

if sys.argv[1] == 'pps':
    post_projection_scatter()
elif sys.argv[1] == 'lt':
    legendre_transform(2)
elif sys.argv[1] == 'ltmulti':
    legendre_transform(12)
elif sys.argv[1] == 'lp':
    legendre_precompute()
