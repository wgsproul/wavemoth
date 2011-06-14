from __future__ import division
from cpython cimport PyBytes_FromStringAndSize
from libc.stdlib cimport free
from libc.string cimport memcpy

cdef extern from "malloc.h":
    void *memalign(size_t boundary, size_t size)

cdef extern from "butterfly.h":
    ctypedef int bfm_index_t
    ctypedef int int32_t
    ctypedef int int64_t

    int bfm_apply_d(char *matrixdata, double *x, double *y,
                    bfm_index_t nrows, bfm_index_t ncols, bfm_index_t nvecs)

cdef extern from "blas.h":
    void dgemm_crr(double *A, double *X, double *Y,
                   int32_t m, int32_t n, int32_t k,
                   double beta)
    void dgemm_rrr(double *A, double *X, double *Y,
                   int32_t m, int32_t n, int32_t k,
                   double beta)

from io import BytesIO
import numpy as np
cimport numpy as np
from interpolative_decomposition import sparse_interpolative_decomposition

if sizeof(bfm_index_t) == 4:
    index_dtype = np.int32
elif sizeof(bfm_index_t) == 8:
    index_dtype = np.int64
else:
    assert False

class ButterflyMatrixError(Exception):
    pass

cdef class SerializedMatrix:
    cdef char *buf
    cdef bint owns_data
    cdef object matrixdata
    cdef public int nrows, ncols
    cdef size_t buflen
    
    def __cinit__(self, bytes matrixdata, bfm_index_t nrows, bfm_index_t ncols):
        self.owns_data = <Py_ssize_t><char*>matrixdata % 16 != 0
        if self.owns_data:
            self.buf = <char*>memalign(16, len(matrixdata))
            memcpy(self.buf, <char*>matrixdata, len(matrixdata))
            self.matrixdata = None
        else:
            self.buf = <char*>matrixdata
            self.matrixdata = matrixdata
        self.buflen = len(matrixdata)
        self.nrows = nrows
        self.ncols = ncols

    def __dealloc__(self):
        if self.owns_data:
            free(self.buf)
            self.owns_data = False

    def __reduce__(self):
        matrixdata = PyBytes_FromStringAndSize(self.buf, self.buflen)
        return (SerializedMatrix, (matrixdata, self.nrows, self.ncols))

    def apply(self, vec, out=None, repeats=1):
        vec = np.ascontiguousarray(vec, dtype=np.double)
        orig_ndim = vec.ndim
        if vec.ndim == 1:
            vec = vec.reshape((vec.shape[0], 1))
        elif vec.ndim > 2:
            raise ValueError()
        if vec.shape[0] != self.ncols:
            raise ValueError("Matrix does not conform to vector")
        if out is None:
            out = np.zeros((self.nrows, vec.shape[1]), dtype=np.double)

        cdef np.ndarray[double, ndim=2, mode='c'] vec_ = vec
        cdef np.ndarray[double, ndim=2, mode='c'] out_ = out

        cdef int i
        for i in range(repeats):
            ret = bfm_apply_d(self.buf, <double*>vec_.data,
                              <double*>out_.data, self.nrows, self.ncols, vec.shape[1])
        if ret != 0:
            raise ButterflyMatrixError('Error code %d' % ret)
        if orig_ndim == 1:
            out = out[:, 0]
        return out

    def digest(self):
        import hashlib
        d = hashlib.sha1()
        d.update(self.get_data())
        return d.hexdigest()

    def get_data(self):
        return PyBytes_FromStringAndSize(self.buf, self.buflen)


cdef write_bin(stream, char *buf, Py_ssize_t size):
    stream.write(PyBytes_FromStringAndSize(buf, size))

def write_int32(stream, int32_t i):
    write_bin(stream, <char*>&i, sizeof(i))

def write_int64(stream, int64_t i):
    write_bin(stream, <char*>&i, sizeof(i))

def write_index_t(stream, bfm_index_t i):
    write_bin(stream, <char*>&i, sizeof(i))

def write_array(stream, arr):
    n = stream.write(bytes(arr.data))
    #assert n == np.prod(arr.shape) * arr.itemsize

def pad128(stream):
    i = stream.tell()
    m = i % 16
    if m != 0:
        stream.write(b'\0' * (16 - m))

class IdentityNode(object):
    def __init__(self, n):
        self.ncols = self.nrows = n
        self.block_heights = [n]
        
    def write_to_stream(self, stream):
        write_index_t(stream, self.ncols)

    def apply(self, x):
        return x

    def size(self):
        return 0

def unroll_pairs(pairs):
    result = []
    for a, b in pairs:
        result.append(a)
        result.append(b)
    return result

def format_numbytes(x):
    if x < 1024**2:
        units = 'KB'
        x /= 1024
    elif x < 1024**3:
        units = 'MB'
        x /= 1024**2
    else:
        units = 'GB'
        x /= 1024**3
    return '%.1f %s' % (x, units)

class RootNode(object):
    def __init__(self, D_blocks, S_node):
        self.D_blocks = [np.ascontiguousarray(D, dtype=np.double)
                         for D in D_blocks]
        if len(self.D_blocks) != 2 * len(S_node.blocks):
            raise ValueError("Wrong number of diagonal blocks w.r.t. S_node")
        self.S_node = S_node
        self.nrows = sum(block.shape[0] for block in self.D_blocks)
        self.ncols = S_node.ncols
        for D, ip in zip(self.D_blocks, unroll_pairs(self.S_node.blocks)):
            if D.shape[1] != ip.shape[0]:
                raise ValueError('Nonconforming matrices')

    def write_to_stream(self, stream):
        # We write both D_blocks and the first S_node interleaved
        # HEADER
        write_index_t(stream, len(self.D_blocks) // 2) # order field
        write_index_t(stream, self.nrows)
        write_index_t(stream, self.ncols)
        # ROOT NODE
        block_heights = np.asarray(
            [D.shape[0] for D in self.D_blocks], dtype=index_dtype)
        write_array(stream, block_heights)
        L, R = self.S_node.children
        write_index_t(stream, L.nrows)
        write_index_t(stream, R.nrows)
        write_index_t(stream, L.ncols)
        L.write_to_stream(stream)
        R.write_to_stream(stream)
        
        for D, ip_block in zip(self.D_blocks, unroll_pairs(self.S_node.blocks)):
            write_index_t(stream, D.shape[1])
            ip_block.write_to_stream(stream)
            pad128(stream)
            write_array(stream, D)

    def apply(self, x):
        y = self.S_node.apply(x)
        out = np.empty(self.nrows, np.double)
        i_out = 0
        i_y = 0
        for block in self.D_blocks:
            m, n = block.shape
            out[i_out:i_out + m] = np.dot(block, y[i_y:i_y + n])
            i_out += m
            i_y += n
        return out

    def size(self):
        return (self.S_node.size() +
                sum(np.prod(block.shape) for block in self.D_blocks))

    def get_stats(self):
        if self.nrows == 0 or self.ncols == 0:
            return "empty"
        Plms_size = sum(np.prod(block.shape) for block in self.D_blocks)
        size = self.size()
        dense = self.nrows * self.ncols
        return "compression %.2f -> %s (%.2f -> %s Plms)" % (
            size / dense,
            format_numbytes(size * 8),
            Plms_size / size,
            format_numbytes(Plms_size * 8)
            )

class InterpolationBlock(object):
    def __init__(self, filter, interpolant):
        self.filter = np.ascontiguousarray(filter, dtype=np.int8)
        self.interpolant = np.ascontiguousarray(interpolant, dtype=np.double)
        n = self.filter.shape[0]
        k = n - np.sum(self.filter)
        self.shape = (self.interpolant.shape[0], n)
        if self.interpolant.shape[1] != n - k:
            raise ValueError("interpolant.shape[1] != n - k")
        if self.interpolant.shape[0] != k:
            raise ValueError("interpolant.shape[0] != k")
        
    def write_to_stream(self, stream):
        write_array(stream, self.filter)
        pad128(stream)
        write_array(stream, self.interpolant)

    def apply(self, x):
        y = x[self.filter == 0, :]
        y += np.dot(self.interpolant, x[self.filter == 1, :])
        return y

    def size(self):
        return np.prod(self.interpolant.shape)
    
class InnerNode(object):
    def __init__(self, blocks, children):
        if 2**int(np.log2(len(blocks))) != len(blocks):
            raise ValueError("len(blocks) not a power of 2")
        if len(children) != 2:
            raise ValueError("len(children) != 2")
        L, R = children
        for (T_ip, B_ip), lh, rh in zip(blocks, L.block_heights, R.block_heights):
            if not (T_ip.shape[1] == B_ip.shape[1] == lh + rh):
                print T_ip.shape, B_ip.shape, lh, rh
                raise ValueError("Nonconforming matrices")
        self.blocks = blocks
        self.ncols = sum(child.ncols for child in children)
        self.children = children
        self.block_heights = sum([[T_ip.shape[0], B_ip.shape[0]]
                                 for T_ip, B_ip in blocks], [])
        self.nrows = sum(self.block_heights)

    def write_to_stream(self, stream):
        L, R = self.children
        write_array(stream, np.asarray(self.block_heights, dtype=index_dtype))
        write_index_t(stream, L.nrows)
        write_index_t(stream, R.nrows)
        write_index_t(stream, L.ncols)
        L.write_to_stream(stream)
        R.write_to_stream(stream)
        for T_ip, B_ip in self.blocks:
            T_ip.write_to_stream(stream)
            B_ip.write_to_stream(stream)

    def apply(self, x):
        # z is the vector containing the contiguous result of the 2 children
        # The permutation happens in reading from z in a permuted way; so each
        # butterfly application permutes its input
        # Recurse to children butterflies
        LS, RS = self.children
        assert x.shape[0] == self.ncols
        assert x.shape[0] == LS.ncols + RS.ncols
        z_left = LS.apply(x[:LS.ncols])
        z_right = RS.apply(x[LS.ncols:])

        # Apply this butterfly, permuting the input as we go
        y = np.empty(self.nrows, np.double)
        i_y = 0
        i_l = i_r = 0
        for i_block, (T_ip, B_ip) in enumerate(self.blocks):
            # Merge together input
            lw = LS.block_heights[i_block]
            rw = RS.block_heights[i_block]
            assert T_ip.shape[1] == B_ip.shape[1] == lw + rw
            buf = np.empty(lw + rw)
            buf[:lw] = z_left[i_l:i_l + lw]
            i_l += lw
            buf[lw:] = z_right[i_r:i_r + rw]
            i_r += rw
            # Do computation
            th = self.block_heights[2 * i_block]
            bh = self.block_heights[2 * i_block + 1]
            assert T_ip.shape[0] == th and B_ip.shape[0] == bh
            y[i_y:i_y + th] = T_ip.apply(buf)
            i_y += th
            y[i_y:i_y + bh] = B_ip.apply(buf)
            i_y += bh
        return y
        
    def size(self):
        return sum([child.size() for child in self.children] +
                   [A_ip.size() + B_ip.size()
                    for A_ip, B_ip in self.blocks])
        

def permutations_to_filter(alst, blst):
    filter = np.zeros(len(alst) + len(blst), dtype=np.int8)
    filter[blst] = 1
    return filter

def matrix_interpolative_decomposition(A, eps):
    iden_list, ipol_list, A_k, A_ip = sparse_interpolative_decomposition(A, eps)
    filter = permutations_to_filter(iden_list, ipol_list)
    return A_k, InterpolationBlock(filter, A_ip)

def butterfly_core(A_k_blocks, eps):
    if len(A_k_blocks) == 1:
        # No compression achieved anyway at this stage when split
        # into odd l/even l
        return A_k_blocks, IdentityNode(A_k_blocks[0].shape[1])
    mid = len(A_k_blocks) // 2
    left_blocks, left_node = butterfly_core(A_k_blocks[:mid], eps)
    right_blocks, right_node = butterfly_core(A_k_blocks[mid:], eps)
    out_blocks = []
    out_interpolants = []
    for L, R in zip(left_blocks, right_blocks):
        # Horizontal join
        LR = np.hstack([L, R])
        # Vertical split & compress
        vmid = LR.shape[0] // 2
        T = LR[:vmid, :]
        B = LR[vmid:, :]
        T_k, T_ip = matrix_interpolative_decomposition(T, eps)
        B_k, B_ip = matrix_interpolative_decomposition(B, eps)
        assert T_ip.shape[1] == B_ip.shape[1] == LR.shape[1]       
        out_interpolants.append((T_ip, B_ip))
        out_blocks.append(T_k)
        out_blocks.append(B_k)
    return out_blocks, InnerNode(out_interpolants, (left_node, right_node))

def get_number_of_levels(n, min):
    levels = 0
    while True:
        n //= 2
        if n < min:
            break
        levels += 1
    return levels

def partition_columns(X, numlevels):
    if numlevels == 0:
        return [X]
    else:
        hmid = X.shape[1] // 2
        return (partition_columns(X[:, :hmid], numlevels - 1) +
                partition_columns(X[:, hmid:], numlevels - 1))

def butterfly_compress(A, min_rows=16, eps=1e-10):
    numlevels = get_number_of_levels(A.shape[0], min_rows)
    B_list = partition_columns(A, numlevels)
    diagonal_blocks, S_tree = butterfly_core(B_list, eps)
    if isinstance(S_tree, IdentityNode):
        n = S_tree.ncols
        filter = np.zeros(S_tree.ncols)
        IP1 = InterpolationBlock(filter, np.zeros((n, 0)))
        IP2 = InterpolationBlock(np.ones(n), np.zeros((0, n)))
        I1 = IdentityNode(n)
        I2 = IdentityNode(0)
        S_tree = InnerNode([(IP1, IP2)], (I1, I2))
        diagonal_blocks.append(np.zeros((0, 0)))
    result = RootNode(diagonal_blocks, S_tree)
    return result

def serialize_butterfly_matrix(M):
    data = BytesIO()
    M.write_to_stream(data)
    return SerializedMatrix(data.getvalue(), M.nrows, M.ncols)

def benchmark_dgemm_crr(np.ndarray[double, ndim=2, mode='fortran'] A,
                        np.ndarray[double, ndim=2, mode='c'] X,
                        np.ndarray[double, ndim=2, mode='c'] Y,
                        int repeats=0):
    assert A.shape[1] == X.shape[0]
    assert A.shape[0] == Y.shape[0]
    cdef int i
    for i in range(repeats):
        dgemm_crr(<double*>A.data, <double*>X.data, <double*>Y.data,
                  Y.shape[0], Y.shape[1], A.shape[1], 0.0)
   
def benchmark_dgemm_rrr(np.ndarray[double, ndim=2, mode='c'] A,
                        np.ndarray[double, ndim=2, mode='c'] X,
                        np.ndarray[double, ndim=2, mode='c'] Y,
                        int repeats=0):
    assert A.shape[1] == X.shape[0]
    assert A.shape[0] == Y.shape[0]
    cdef int i
    for i in range(repeats):
        dgemm_rrr(<double*>A.data, <double*>X.data, <double*>Y.data,
                  Y.shape[0], Y.shape[1], A.shape[1], 0.0)

    
