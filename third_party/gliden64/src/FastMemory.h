#ifndef FAST_MEMORY_H
#define FAST_MEMORY_H

// Fills and copies for RDRAM that is not in the cache, which is the usual
// case for frame buffers and texture data. On the Wii U's Espresso a store to
// an uncached line first reads the line from memory, and each read waits for
// the previous one. dcbz claims a whole 32-byte destination line without
// reading it, and dcbt starts reading source lines ahead of the copy. Other
// hosts use the standard functions.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include "Types.h"

// Writes _count copies of _value to _dst.
inline void fillWords(u32 * _dst, u32 _count, u32 _value)
{
#ifdef __WIIU__
	u32 * const end = _dst + _count;
	while (_dst < end && (reinterpret_cast<uintptr_t>(_dst) & 31) != 0)
		*_dst++ = _value;
	while (end - _dst >= 8) {
		__asm__ volatile("dcbz 0,%0" : : "r"(_dst) : "memory");
		for (u32 i = 0; i < 8; ++i)
			_dst[i] = _value;
		_dst += 8;
	}
	while (_dst < end)
		*_dst++ = _value;
#else
	std::fill(_dst, _dst + _count, _value);
#endif
}

// memcpy for buffers that do not overlap. Use it for large copies out of
// uncached RDRAM: for TMEM loads (small, destination already cached) it made
// load block 30% slower on the console (2026-10-13).
inline void copyBytes(void * _dst, const void * _src, size_t _size)
{
#ifdef __WIIU__
	u8 * dst = static_cast<u8*>(_dst);
	const u8 * src = static_cast<const u8*>(_src);
	size_t head = (0 - reinterpret_cast<uintptr_t>(dst)) & 31;
	if (head > _size)
		head = _size;
	memcpy(dst, src, head);
	dst += head;
	src += head;
	_size -= head;
	// dcbt is a hint and never faults, so reading ahead past the end is safe.
	static const size_t kPrefetch = 128;
	while (_size >= 32) {
		__asm__ volatile("dcbt 0,%0" : : "r"(src + kPrefetch));
		__asm__ volatile("dcbz 0,%0" : : "r"(dst) : "memory");
		memcpy(dst, src, 32);
		dst += 32;
		src += 32;
		_size -= 32;
	}
	memcpy(dst, src, _size);
#else
	memcpy(_dst, _src, _size);
#endif
}

// An allocator whose resize leaves new elements uninitialized, for buffers
// that are overwritten right after they are sized.
template <typename T>
struct UninitializedAllocator : std::allocator<T>
{
	template <typename U> struct rebind { typedef UninitializedAllocator<U> other; };
	UninitializedAllocator() = default;
	template <typename U> UninitializedAllocator(const UninitializedAllocator<U> &) {}
	template <typename U> void construct(U * _p) { ::new (static_cast<void*>(_p)) U; }
	template <typename U, typename... Args> void construct(U * _p, Args &&... _args)
	{
		::new (static_cast<void*>(_p)) U(std::forward<Args>(_args)...);
	}
};

#endif // FAST_MEMORY_H
