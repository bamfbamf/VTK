#ifndef ZFP_ARRAY3_H
#define ZFP_ARRAY3_H

#include <cstddef>
#include <iterator>
#include "zfparray.h"
#include "zfpcodec.h"
#include "zfp/cache.h"

namespace zfp {

// compressed 3D array of scalars
template < typename Scalar, class Codec = zfp::codec<Scalar> >
class array3 : public array {
public:
  array3() : array(3, Codec::type), cache(0) {}

  // constructor of nx * ny * nz array using rate bits per value, at least
  // csize bytes of cache, and optionally initialized from flat array p
  array3(uint nx, uint ny, uint nz, double rate, const Scalar* p = 0, size_t csize = 0) :
    array(3, Codec::type),
    cache(lines(csize, nx, ny, nz))
  {
    set_rate(rate);
    resize(nx, ny, nz, p == 0);
    if (p)
      set(p);
  }

  // total number of elements in array
  size_t size() const { return size_t(nx) * size_t(ny) * size_t(nz); }

  // array dimensions
  uint size_x() const { return nx; }
  uint size_y() const { return ny; }
  uint size_z() const { return nz; }

  // resize the array (all previously stored data will be lost)
  void resize(uint nx, uint ny, uint nz, bool clear = true)
  {
    if (nx == 0 || ny == 0 || nz == 0)
      free();
    else {
      this->nx = nx;
      this->ny = ny;
      this->nz = nz;
      bx = (nx + 3) / 4;
      by = (ny + 3) / 4;
      bz = (nz + 3) / 4;
      blocks = bx * by * bz;
      alloc(clear);

      // precompute block dimensions
      deallocate(shape);
      if ((nx | ny | nz) & 3u) {
        shape = (uchar*)allocate(blocks);
        uchar* p = shape;
        for (uint k = 0; k < bz; k++)
          for (uint j = 0; j < by; j++)
            for (uint i = 0; i < bx; i++)
              *p++ = (i == bx - 1 ? -nx & 3u : 0) + 4 * ((j == by - 1 ? -ny & 3u : 0) + 4 * (k == bz - 1 ? -nz & 3u : 0));
      }
      else
        shape = 0;
    }
  }

  // cache size in number of bytes
  size_t cache_size() const { return cache.size() * sizeof(CacheLine); }

  // set minimum cache size in bytes (array dimensions must be known)
  void set_cache_size(size_t csize)
  {
    flush_cache();
    cache.resize(lines(csize, nx, ny, nz));
  }

  // empty cache without compressing modified cached blocks
  void clear_cache() const { cache.clear(); }

  // flush cache by compressing all modified cached blocks
  void flush_cache() const
  {
    for (typename Cache<CacheLine>::const_iterator p = cache.first(); p; p++) {
      if (p->tag.dirty()) {
        uint b = p->tag.index() - 1;
        encode(b, p->line->a);
      }
      cache.flush(p->line);
    }
  }

  // decompress array and store at p
  void get(Scalar* p) const
  {
    uint b = 0;
    for (uint k = 0; k < bz; k++, p += 4 * nx * (ny - by))
      for (uint j = 0; j < by; j++, p += 4 * (nx - bx))
        for (uint i = 0; i < bx; i++, p += 4, b++) {
          const CacheLine* line = cache.lookup(b + 1);
          if (line)
            line->get(p, 1, nx, nx * ny, shape ? shape[b] : 0);
          else
            decode(b, p, 1, nx, nx * ny);
        }
  }

  // initialize array by copying and compressing data stored at p
  void set(const Scalar* p)
  {
    uint b = 0;
    for (uint k = 0; k < bz; k++, p += 4 * nx * (ny - by))
      for (uint j = 0; j < by; j++, p += 4 * (nx - bx))
        for (uint i = 0; i < bx; i++, p += 4, b++)
          encode(b, p, 1, nx, nx * ny);
    cache.clear();
  }

  class pointer;

  // reference to a single array value
  class reference {
  public:
    operator Scalar() const { return array->get(i, j, k); }
    reference operator=(const reference& r) { array->set(i, j, k, r.operator Scalar()); return *this; }
    reference operator=(Scalar val) { array->set(i, j, k, val); return *this; }
    reference operator+=(Scalar val) { array->add(i, j, k, val); return *this; }
    reference operator-=(Scalar val) { array->sub(i, j, k, val); return *this; }
    reference operator*=(Scalar val) { array->mul(i, j, k, val); return *this; }
    reference operator/=(Scalar val) { array->div(i, j, k, val); return *this; }
    pointer operator&() const { return pointer(*this); }
    // swap two array elements via proxy references
    friend void swap(reference a, reference b)
    {
      Scalar x = a.operator Scalar();
      Scalar y = b.operator Scalar();
      b.operator=(x);
      a.operator=(y);
    }
  protected:
    friend class array3;
    friend class iterator;
    explicit reference(array3* array, uint i, uint j, uint k) : array(array), i(i), j(j), k(k) {}
    array3* array;
    uint i, j, k;
  };

  // pointer to a single value in flattened array
  class pointer {
  public:
    pointer() : ref(0, 0, 0, 0) {}
    pointer operator=(const pointer& p) { ref.array = p.ref.array; ref.i = p.ref.i; ref.j = p.ref.j; ref.k = p.ref.k; return *this; }
    reference operator*() const { return ref; }
    reference operator[](ptrdiff_t d) const { return *operator+(d); }
    pointer& operator++() { increment(); return *this; }
    pointer& operator--() { decrement(); return *this; }
    pointer operator++(int) { pointer p = *this; increment(); return p; }
    pointer operator--(int) { pointer p = *this; decrement(); return p; }
    pointer operator+=(ptrdiff_t d) { set(index() + d); return *this; }
    pointer operator-=(ptrdiff_t d) { set(index() - d); return *this; }
    pointer operator+(ptrdiff_t d) const { pointer p = *this; p += d; return p; }
    pointer operator-(ptrdiff_t d) const { pointer p = *this; p -= d; return p; }
    ptrdiff_t operator-(const pointer& p) const { return index() - p.index(); }
    bool operator==(const pointer& p) const { return ref.array == p.ref.array && ref.i == p.ref.i && ref.j == p.ref.j && ref.k == p.ref.k; }
    bool operator!=(const pointer& p) const { return !operator==(p); }
  protected:
    friend class array3;
    friend class reference;
    explicit pointer(reference r) : ref(r) {}
    explicit pointer(array3* array, uint i, uint j, uint k) : ref(array, i, j, k) {}
    ptrdiff_t index() const { return ref.i + ref.array->nx * (ref.j + ref.array->ny * ref.k); }
    void set(ptrdiff_t index) { ref.array->ijk(ref.i, ref.j, ref.k, index); }
    void increment()
    {
      if (++ref.i == ref.array->nx) {
        ref.i = 0;
        if (++ref.j == ref.array->ny) {
          ref.j = 0;
          ref.k++;
        }
      }
    }
    void decrement()
    {
      if (!ref.i--) {
        ref.i = ref.array->nx - 1;
        if (!ref.j--) {
          ref.j = ref.array->ny - 1;
          ref.k--;
        }
      }
    }
    reference ref;
  };

  // forward iterator that visits array block by block
  class iterator {
  public:
    // typedefs for STL compatibility
    typedef Scalar value_type;
    typedef ptrdiff_t difference_type;
    typedef typename array3::reference reference;
    typedef typename array3::pointer pointer;
    typedef std::forward_iterator_tag iterator_category;

    iterator() : ref(0, 0, 0, 0) {}
    iterator operator=(const iterator& it) { ref.array = it.ref.array; ref.i = it.ref.i; ref.j = it.ref.j; ref.k = it.ref.k; return *this; }
    reference operator*() const { return ref; }
    iterator& operator++() { increment(); return *this; }
    iterator operator++(int) { iterator it = *this; increment(); return it; }
    bool operator==(const iterator& it) const { return ref.array == it.ref.array && ref.i == it.ref.i && ref.j == it.ref.j && ref.k == it.ref.k; }
    bool operator!=(const iterator& it) const { return !operator==(it); }
    uint i() const { return ref.i; }
    uint j() const { return ref.j; }
    uint k() const { return ref.k; }
  protected:
    friend class array3;
    explicit iterator(array3* array, uint i, uint j, uint k) : ref(array, i, j, k) {}
    void increment()
    {
      ref.i++;
      if (!(ref.i & 3u) || ref.i == ref.array->nx) {
        ref.i = (ref.i - 1) & ~3u;
        ref.j++;
        if (!(ref.j & 3u) || ref.j == ref.array->ny) {
          ref.j = (ref.j - 1) & ~3u;
          ref.k++;
          if (!(ref.k & 3u) || ref.k == ref.array->nz) {
            ref.k = (ref.k - 1) & ~3u;
            // done with block; advance to next
            if ((ref.i += 4) >= ref.array->nx) {
              ref.i = 0;
              if ((ref.j += 4) >= ref.array->ny) {
                ref.j = 0;
                if ((ref.k += 4) >= ref.array->nz)
                  ref.k = ref.array->nz;
              }
            }
          }
        }
      }
    }
    reference ref;
  };

  // (i, j, k) accessors
  const Scalar& operator()(uint i, uint j, uint k) const { return get(i, j, k); }
  reference operator()(uint i, uint j, uint k) { return reference(this, i, j, k); }

  // flat index accessors
  const Scalar& operator[](uint index) const
  {
    uint i, j, k;
    ijk(i, j, k, index);
    return get(i, j, k);
  }
  reference operator[](uint index)
  {
    uint i, j, k;
    ijk(i, j, k, index);
    return reference(this, i, j, k);
  }

  // sequential iterators
  iterator begin() { return iterator(this, 0, 0, 0); }
  iterator end() { return iterator(this, 0, 0, nz); }

protected:
  // cache line representing one block of decompressed values
  class CacheLine {
  public:
    friend class array3;
    const Scalar& operator()(uint i, uint j, uint k) const { return a[index(i, j, k)]; }
    Scalar& operator()(uint i, uint j, uint k) { return a[index(i, j, k)]; }
    // copy cache line
    void get(Scalar* p, int sx, int sy, int sz) const
    {
      const Scalar* q = a;
      for (uint z = 0; z < 4; z++, p += sz - 4 * sy)
        for (uint y = 0; y < 4; y++, p += sy - 4 * sx)
          for (uint x = 0; x < 4; x++, p += sx, q++)
            *p = *q;
    }
    void get(Scalar* p, int sx, int sy, int sz, uint shape) const
    {
      if (!shape)
        get(p, sx, sy, sz);
      else {
        // determine block dimensions
        uint nx = 4 - (shape & 3u); shape >>= 2;
        uint ny = 4 - (shape & 3u); shape >>= 2;
        uint nz = 4 - (shape & 3u); shape >>= 2;
        const Scalar* q = a;
        for (uint z = 0; z < nz; z++, p += sz - ny * sy, q += 16 - 4 * ny)
          for (uint y = 0; y < ny; y++, p += sy - nx * sx, q += 4 - nx)
            for (uint x = 0; x < nx; x++, p += sx, q++)
              *p = *q;
      }
    }
  protected:
    static uint index(uint i, uint j, uint k) { return (i & 3u) + 4 * ((j & 3u) + 4 * (k & 3u)); }
    Scalar a[64];
  };

  // inspector
  const Scalar& get(uint i, uint j, uint k) const
  {
    CacheLine* p = line(i, j, k, false);
    return (*p)(i, j, k);
  }

  // mutator
  void set(uint i, uint j, uint k, Scalar val)
  {
    CacheLine* p = line(i, j, k, true);
    (*p)(i, j, k) = val;
  }

  // in-place updates
  void add(uint i, uint j, uint k, Scalar val) { (*line(i, j, k, true))(i, j, k) += val; }
  void sub(uint i, uint j, uint k, Scalar val) { (*line(i, j, k, true))(i, j, k) -= val; }
  void mul(uint i, uint j, uint k, Scalar val) { (*line(i, j, k, true))(i, j, k) *= val; }
  void div(uint i, uint j, uint k, Scalar val) { (*line(i, j, k, true))(i, j, k) /= val; }

  // return cache line for (i, j, k); may require write-back and fetch
  CacheLine* line(uint i, uint j, uint k, bool write) const
  {
    CacheLine* p = 0;
    uint b = block(i, j, k);
    typename Cache<CacheLine>::Tag t = cache.access(p, b + 1, write);
    uint c = t.index() - 1;
    if (c != b) {
      // write back occupied cache line if it is dirty
      if (t.dirty())
        encode(c, p->a);
      // fetch cache line
      decode(b, p->a);
    }
    return p;
  }

  // encode block with given index
  void encode(uint index, const Scalar* block) const
  {
    stream_wseek(stream->stream, index * blkbits);
    Codec::encode_block_3(stream, block, shape ? shape[index] : 0);
    stream_flush(stream->stream);
  }

  // encode block with given index from strided array
  void encode(uint index, const Scalar* p, int sx, int sy, int sz) const
  {
    stream_wseek(stream->stream, index * blkbits);
    Codec::encode_block_strided_3(stream, p, shape ? shape[index] : 0, sx, sy, sz);
    stream_flush(stream->stream);
  }

  // decode block with given index
  void decode(uint index, Scalar* block) const
  {
    stream_rseek(stream->stream, index * blkbits);
    Codec::decode_block_3(stream, block, shape ? shape[index] : 0);
  }

  // decode block with given index to strided array
  void decode(uint index, Scalar* p, int sx, int sy, int sz) const
  {
    stream_rseek(stream->stream, index * blkbits);
    Codec::decode_block_strided_3(stream, p, shape ? shape[index] : 0, sx, sy, sz);
  }

  // block index for (i, j, k)
  uint block(uint i, uint j, uint k) const { return (i / 4) + bx * ((j / 4) + by * (k / 4)); }

  // convert flat index to (i, j, k)
  void ijk(uint& i, uint& j, uint& k, uint index) const
  {
    i = index % nx;
    index /= nx;
    j = index % ny;
    index /= ny;
    k = index;
  }

  // number of cache lines corresponding to size (or suggested size if zero)
  static uint lines(size_t size, uint nx, uint ny, uint nz)
  {
    uint n = uint((size ? size : 8 * nx * ny * sizeof(Scalar)) / sizeof(CacheLine));
    return std::max(n, 1u);
  }

  mutable Cache<CacheLine> cache; // cache of decompressed blocks
};

typedef array3<float> array3f;
typedef array3<double> array3d;

}

#endif
