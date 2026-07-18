// > dist-cutoff-begin
#pragma once
#include "decl_slice.h++"
#include "decl_transcoders.h++"
namespace mdbx {
// < dist-cutoff-end

namespace allocation_aware_details {

template <typename A> constexpr bool allocator_is_always_equal() noexcept {
#if defined(__cpp_lib_allocator_traits_is_always_equal) && __cpp_lib_allocator_traits_is_always_equal >= 201411L
  return ::std::allocator_traits<A>::is_always_equal::value;
#else
  return ::std::is_empty<A>::value;
#endif /* __cpp_lib_allocator_traits_is_always_equal */
}

template <typename T, typename A = typename T::allocator_type,
          bool PoCMA = ::std::allocator_traits<A>::propagate_on_container_move_assignment::value>
struct move_assign_alloc;

template <typename T, typename A> struct move_assign_alloc<T, A, false> {
  static constexpr bool is_always_equal() noexcept { return allocator_is_always_equal<A>(); }
  static constexpr bool is_nothrow() noexcept { return is_always_equal(); }
  static MDBX_CXX20_CONSTEXPR bool is_moveable(T *target, T &source) noexcept {
    if MDBX_IF_CONSTEXPR (is_always_equal())
      return true;
    else
      return target->get_allocator() == source.get_allocator();
  }
  static MDBX_CXX20_CONSTEXPR void propagate(T *, T &) noexcept {}
};

template <typename T, typename A> struct move_assign_alloc<T, A, true> {
  static constexpr bool is_always_equal() noexcept { return allocator_is_always_equal<A>(); }
  static constexpr bool is_nothrow() noexcept { return ::std::is_nothrow_move_assignable<A>::value; }
  static constexpr bool is_moveable(T *, T &) noexcept { return true; }
  static MDBX_CXX20_CONSTEXPR void propagate(T *target, T &source) noexcept {
    target->get_allocator() = ::std::move(source.get_allocator());
  }
};

template <typename T, typename A = typename T::allocator_type,
          bool PoCCA = ::std::allocator_traits<A>::propagate_on_container_copy_assignment::value>
struct copy_assign_alloc;

template <typename T, typename A> struct copy_assign_alloc<T, A, false> {
  static constexpr bool is_always_equal() noexcept { return allocator_is_always_equal<A>(); }
  static constexpr bool is_nothrow() noexcept { return false; }
  static MDBX_CXX20_CONSTEXPR void propagate(T *, const T &) noexcept {}
};

template <typename T, typename A> struct copy_assign_alloc<T, A, true> {
  static constexpr bool is_always_equal() noexcept { return allocator_is_always_equal<A>(); }
  static constexpr bool is_nothrow() noexcept {
    return is_always_equal() || ::std::is_nothrow_copy_assignable<A>::value;
  }
  static MDBX_CXX20_CONSTEXPR void propagate(T *target, const T &source) noexcept(is_nothrow()) {
    if MDBX_IF_CONSTEXPR (!is_always_equal()) {
      if (MDBX_UNLIKELY(target->get_allocator() != source.get_allocator()))
        MDBX_CXX20_UNLIKELY target->get_allocator() =
            ::std::allocator_traits<A>::select_on_container_copy_construction(source.get_allocator());
    } else {
      /* gag for buggy compilers */
      (void)target;
      (void)source;
    }
  }
};

template <typename T, typename A = typename T::allocator_type,
          bool PoCS = ::std::allocator_traits<A>::propagate_on_container_swap::value>
struct swap_alloc;

template <typename T, typename A> struct swap_alloc<T, A, false> {
  static constexpr bool is_always_equal() noexcept { return allocator_is_always_equal<A>(); }
  static constexpr bool is_nothrow() noexcept { return is_always_equal(); }
  static MDBX_CXX20_CONSTEXPR void propagate(T &left, T &right) noexcept(is_nothrow()) {
    if MDBX_IF_CONSTEXPR (!is_always_equal()) {
      if (MDBX_UNLIKELY(left.get_allocator() != right.get_allocator()))
        MDBX_CXX20_UNLIKELY throw_allocators_mismatch();
    } else {
      /* gag for buggy compilers */
      (void)left;
      (void)right;
    }
  }
};

template <typename T, typename A> struct swap_alloc<T, A, true> {
  static constexpr bool is_always_equal() noexcept { return allocator_is_always_equal<A>(); }
  static constexpr bool is_nothrow() noexcept {
    return is_always_equal() ||
#if defined(__cpp_lib_is_swappable) && __cpp_lib_is_swappable >= 201603L
           ::std::is_nothrow_swappable<A>() ||
#endif /* __cpp_lib_is_swappable >= 201603L */
           (::std::is_nothrow_move_constructible<A>::value && ::std::is_nothrow_move_assignable<A>::value);
  }
  static MDBX_CXX20_CONSTEXPR void propagate(T &left, T &right) noexcept(is_nothrow()) {
    if MDBX_IF_CONSTEXPR (!is_always_equal())
      MDBX_CXX20_UNLIKELY ::std::swap(left.get_allocator(), right.get_allocator());
    else {
      /* gag for buggy compilers */
      (void)left;
      (void)right;
    }
  }
};

} // namespace allocation_aware_details

struct default_capacity_policy {
  enum : size_t {
    extra_inplace_storage = 0,
    inplace_storage_size_rounding = 16,
    pettiness_threshold = 64,
    max_reserve = 65536
  };

  static MDBX_CXX11_CONSTEXPR size_t round(const size_t value) {
    static_assert((pettiness_threshold & (pettiness_threshold - 1)) == 0, "pettiness_threshold must be a power of 2");
    static_assert(pettiness_threshold >= sizeof(uint64_t), "pettiness_threshold must be > 7");
    constexpr const auto pettiness_mask = ~size_t(pettiness_threshold - 1);
    return (value + pettiness_threshold - 1) & pettiness_mask;
  }

  static MDBX_CXX11_CONSTEXPR size_t advise(const size_t current, const size_t wanna, const size_t inplace) {
    static_assert(max_reserve % pettiness_threshold == 0, "max_reserve must be a multiple of pettiness_threshold");
    static_assert(max_reserve / 3 > pettiness_threshold, "max_reserve must be > pettiness_threshold * 3");

    if (wanna <= inplace && (current <= inplace || current >= std::max(inplace + inplace, size_t(pettiness_threshold))))
      return inplace;

    if (wanna > current)
      /* doubling capacity, but don't made reserve more than max_reserve */
      return round(wanna + ::std::min(size_t(max_reserve), current));

    if (current - wanna >
        /* shrink if reserve will more than half of current or max_reserve,
         * but not less than pettiness_threshold */
        ::std::min(wanna + pettiness_threshold, size_t(max_reserve)))
      return round(wanna);

    /* keep unchanged */
    return current;
  }
};

/// \brief Type tag for delivered buffer template classes.
struct buffer_tag {};

/// \brief The chunk of data stored inside the buffer or located outside it.
template <class ALLOCATOR, typename CAPACITY_POLICY>
class MDBX_MSVC_DECLSPEC_EMPTY_BASES buffer : public slice, public buffer_tag {
public:
  using inherited = slice;
#if !defined(_MSC_VER) || _MSC_VER > 1900
  using allocator_type = typename ::std::allocator_traits<ALLOCATOR>::template rebind_alloc<uint64_t>;
#else
  using allocator_type = typename ALLOCATOR::template rebind<uint64_t>::other;
#endif /* MSVC is mad */
  using allocator_traits = ::std::allocator_traits<allocator_type>;
  using reservation_policy = CAPACITY_POLICY;
  enum : size_t {
    max_length = MDBX_MAXDATASIZE,
    max_capacity = (max_length / 3u * 4u + 1023u) & ~size_t(1023),
    extra_inplace_storage = reservation_policy::extra_inplace_storage,
    inplace_storage_size_rounding =
        (alignof(max_align_t) * 2 > size_t(reservation_policy::inplace_storage_size_rounding))
            ? alignof(max_align_t) * 2
            : size_t(reservation_policy::inplace_storage_size_rounding),
    pettiness_threshold = reservation_policy::pettiness_threshold
  };

  /// \brief Data storage modality.
  enum class modality { reference, inplace, allocated };

private:
  friend class txn;
  struct silo /* Empty Base Class Optimization */ : public allocator_type {
    MDBX_CXX20_CONSTEXPR const allocator_type &get_allocator() const noexcept { return *this; }
    MDBX_CXX20_CONSTEXPR allocator_type &get_allocator() noexcept { return *this; }

    using allocator_pointer = typename allocator_traits::pointer;
    using allocator_const_pointer = typename allocator_traits::const_pointer;

    MDBX_CXX20_CONSTEXPR ::std::pair<allocator_pointer, size_t> allocate_storage(size_t bytes) {
      MDBX_INLINE_API_ASSERT(bytes >= sizeof(bin));
      constexpr size_t unit = sizeof(typename allocator_type::value_type);
      static_assert((unit & (unit - 1)) == 0, "size of ALLOCATOR::value_type should be a power of 2");
      static_assert(unit > 0, "size of ALLOCATOR::value_type must be > 0");
      const size_t n = (bytes + unit - 1) / unit;
      return ::std::make_pair(allocator_traits::allocate(get_allocator(), n), n * unit);
    }

    MDBX_CXX20_CONSTEXPR void deallocate_storage(allocator_pointer ptr, size_t bytes) {
      constexpr size_t unit = sizeof(typename allocator_type::value_type);
      MDBX_INLINE_API_ASSERT(ptr && bytes >= sizeof(bin) && bytes >= unit && bytes % unit == 0);
      allocator_traits::deallocate(get_allocator(), ptr, bytes / unit);
    }

    MDBX_CXX20_CONSTEXPR ::std::pair<allocator_pointer, size_t> provide_storage(size_t bytes) {
      const size_t capacity = bin::advise_capacity(0, bytes);
      return bin::is_suitable_for_inplace(capacity) ? ::std::pair<allocator_pointer, size_t>(nullptr, capacity)
                                                    : allocate_storage(capacity);
    }

    static MDBX_CXX17_CONSTEXPR void *to_address(allocator_pointer ptr) noexcept {
#if defined(__cpp_lib_to_address) && __cpp_lib_to_address >= 201711L
      return static_cast<void *>(::std::to_address(ptr));
#else
      return static_cast<void *>(::std::addressof(*ptr));
#endif /* __cpp_lib_to_address */
    }

    static MDBX_CXX17_CONSTEXPR const void *to_address(allocator_const_pointer ptr) noexcept {
#if defined(__cpp_lib_to_address) && __cpp_lib_to_address >= 201711L
      return static_cast<const void *>(::std::to_address(ptr));
#else
      return static_cast<const void *>(::std::addressof(*ptr));
#endif /* __cpp_lib_to_address */
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) /* structure was padded due to alignment specifier */
#endif                          /* _MSC_VER */

    union alignas(max_align_t) bin {
      struct stub_allocated_holder /* используется только для вычисления (минимального необходимого) размера,
                                      с учетом выравнивания */
      {
        allocator_pointer stub_ptr_;
        size_t stub_capacity_bytes_;
      };

      enum : byte { lastbyte_poison = 0, lastbyte_inplace_signature = byte(~byte(lastbyte_poison)) };
      enum : size_t {
        inplace_signature_limit = size_t(lastbyte_inplace_signature)
                                  << (sizeof(size_t /* allocated::capacity_bytes_ */) - 1) * CHAR_BIT,
        inplace_size_rounding = size_t(inplace_storage_size_rounding) - 1,
        inplace_size =
            (sizeof(stub_allocated_holder) + extra_inplace_storage + inplace_size_rounding) & ~inplace_size_rounding
      };

      struct capacity_holder {
        byte pad_[inplace_size - sizeof(allocator_pointer)];
        size_t bytes_;
      };

      struct inplace_flag_holder {
        byte buffer_[inplace_size - sizeof(byte)];
        byte lastbyte_;
        MDBX_CXX11_CONSTEXPR inplace_flag_holder(byte signature) : buffer_(), lastbyte_(signature) {}
      };

      allocator_pointer allocated_ptr_;
      capacity_holder capacity_;
      inplace_flag_holder inplace_;

      static constexpr size_t inplace_capacity() noexcept { return sizeof(inplace_flag_holder::buffer_); }
      static constexpr bool is_suitable_for_inplace(size_t capacity_bytes) noexcept {
        static_assert((size_t(reservation_policy::inplace_storage_size_rounding) &
                       (size_t(reservation_policy::inplace_storage_size_rounding) - 1)) == 0,
                      "CAPACITY_POLICY::inplace_storage_size_rounding must be power of 2");
        static_assert(sizeof(bin) == sizeof(inplace_) && sizeof(bin) == sizeof(capacity_), "WTF?");
        return capacity_bytes <= inplace_capacity();
      }

      MDBX_NOTHROW_PURE_FUNCTION constexpr bool is_inplace() const noexcept {
        static_assert(size_t(inplace_signature_limit) > size_t(max_capacity), "WTF?");
        static_assert(std::numeric_limits<size_t>::max() - (std::numeric_limits<size_t>::max() >> CHAR_BIT) ==
                          inplace_signature_limit,
                      "WTF?");
        return inplace_.lastbyte_ == lastbyte_inplace_signature;
      }
      MDBX_NOTHROW_PURE_FUNCTION constexpr bool is_allocated() const noexcept { return !is_inplace(); }

      template <bool destroy_ptr> MDBX_CXX17_CONSTEXPR byte *make_inplace() noexcept {
        if (destroy_ptr) {
          MDBX_CONSTEXPR_ASSERT(is_allocated());
          /* properly destroy allocator::pointer */
          allocated_ptr_.~allocator_pointer();
        }
        inplace_.lastbyte_ = lastbyte_inplace_signature;
        MDBX_CONSTEXPR_ASSERT(is_inplace() && address() == inplace_.buffer_ && is_suitable_for_inplace(capacity()));
        return address();
      }

      template <bool construct_ptr>
      MDBX_CXX17_CONSTEXPR byte *make_allocated(const ::std::pair<allocator_pointer, size_t> &pair) noexcept {
        MDBX_CONSTEXPR_ASSERT(inplace_signature_limit > pair.second);
        if (construct_ptr) {
          MDBX_CONSTEXPR_ASSERT(is_inplace());
          new (&allocated_ptr_) allocator_pointer(pair.first);
        } else {
          MDBX_CONSTEXPR_ASSERT(is_allocated());
          allocated_ptr_ = pair.first;
        }
        capacity_.bytes_ = pair.second;
        MDBX_CONSTEXPR_ASSERT(is_allocated() && address() == to_address(pair.first) && capacity() == pair.second);
        return address();
      }

      MDBX_CXX11_CONSTEXPR bin() noexcept : inplace_(lastbyte_inplace_signature) {
        if (::std::is_trivial<allocator_pointer>::value)
          /* workaround for "uninitialized" warning from some compilers */
          memset(&allocated_ptr_, 0, sizeof(allocated_ptr_));
      }

      static MDBX_CXX20_CONSTEXPR size_t advise_capacity(const size_t current, const size_t wanna) {
        if (MDBX_UNLIKELY(wanna > max_capacity))
          MDBX_CXX20_UNLIKELY throw_max_length_exceeded();

        const size_t advised = reservation_policy::advise(current, wanna, inplace_capacity());
        MDBX_INLINE_API_ASSERT(advised >= wanna);
        return ::std::min(size_t(max_capacity), ::std::max(inplace_capacity(), advised));
      }

      constexpr bool is_inplace(const void *ptr) const noexcept {
        return size_t(static_cast<const byte *>(ptr) - inplace_.buffer_) < inplace_capacity();
      }

      constexpr const byte *address() const noexcept {
        return is_inplace() ? inplace_.buffer_ : static_cast<const byte *>(to_address(allocated_ptr_));
      }
      MDBX_CXX17_CONSTEXPR byte *address() noexcept {
        return is_inplace() ? inplace_.buffer_ : static_cast<byte *>(to_address(allocated_ptr_));
      }
      constexpr size_t capacity() const noexcept { return is_inplace() ? inplace_capacity() : capacity_.bytes_; }
    } bin_;

#ifdef _MSC_VER
#pragma warning(pop)
#endif /* _MSC_VER */

    constexpr bool is_inplace(const void *ptr) const noexcept { return bin_.is_inplace(ptr); }

    MDBX_CXX20_CONSTEXPR void release() noexcept {
      if (bin_.is_allocated()) {
        deallocate_storage(bin_.allocated_ptr_, bin_.capacity_.bytes_);
        /* properly destroy allocator::pointer */
        bin_.allocated_ptr_.~allocator_pointer();
        bin_.inplace_.lastbyte_ = bin::lastbyte_inplace_signature;
      }
    }

    template <bool external_content>
    MDBX_CXX20_CONSTEXPR void *reshape(const size_t wanna_capacity, const size_t wanna_headroom,
                                       const void *const content, const size_t length) {
      MDBX_INLINE_API_ASSERT(wanna_capacity >= wanna_headroom + length);
      const size_t old_capacity = bin_.capacity();
      const size_t new_capacity = bin::advise_capacity(old_capacity, wanna_capacity);
      if (MDBX_LIKELY(new_capacity == old_capacity))
        MDBX_CXX20_LIKELY {
          MDBX_INLINE_API_ASSERT(bin_.is_inplace() == bin::is_suitable_for_inplace(new_capacity));
          byte *const new_place = bin_.address() + wanna_headroom;
          if (MDBX_LIKELY(length))
            MDBX_CXX20_LIKELY {
              if (external_content)
                memcpy(new_place, content, length);
              else {
                const size_t old_headroom = bin_.address() - static_cast<const byte *>(content);
                MDBX_INLINE_API_ASSERT(old_capacity >= old_headroom + length);
                if (MDBX_UNLIKELY(old_headroom != wanna_headroom))
                  MDBX_CXX20_UNLIKELY ::std::memmove(new_place, content, length);
              }
            }
          return new_place;
        }

      if (bin::is_suitable_for_inplace(new_capacity)) {
        MDBX_INLINE_API_ASSERT(bin_.is_allocated());
        const auto old_allocated = ::std::move(bin_.allocated_ptr_);
        /* coverity[USE_AFTER_MOVE] */
        byte *const new_place = bin_.template make_inplace<true>() + wanna_headroom;
        if (MDBX_LIKELY(length))
          MDBX_CXX20_LIKELY memcpy(new_place, content, length);
        deallocate_storage(old_allocated, old_capacity);
        return new_place;
      }

      if (bin_.is_inplace()) {
        const auto pair = allocate_storage(new_capacity);
        MDBX_INLINE_API_ASSERT(pair.second >= new_capacity);
        byte *const new_place = static_cast<byte *>(to_address(pair.first)) + wanna_headroom;
        if (MDBX_LIKELY(length))
          MDBX_CXX20_LIKELY memcpy(new_place, content, length);
        bin_.template make_allocated<true>(pair);
        return new_place;
      }

      const auto old_allocated = ::std::move(bin_.allocated_ptr_);
      if (external_content)
        deallocate_storage(old_allocated, old_capacity);
      const auto pair = allocate_storage(new_capacity);
      MDBX_INLINE_API_ASSERT(pair.second >= new_capacity);
      byte *const new_place = bin_.template make_allocated<false>(pair) + wanna_headroom;
      if (MDBX_LIKELY(length))
        MDBX_CXX20_LIKELY memcpy(new_place, content, length);
      if (!external_content)
        deallocate_storage(old_allocated, old_capacity);
      return new_place;
    }

    MDBX_CXX20_CONSTEXPR const byte *get(size_t offset = 0) const noexcept {
      MDBX_INLINE_API_ASSERT(capacity() >= offset);
      return bin_.address() + offset;
    }
    MDBX_CXX20_CONSTEXPR byte *get(size_t offset = 0) noexcept {
      MDBX_INLINE_API_ASSERT(capacity() >= offset);
      return bin_.address() + offset;
    }
    MDBX_CXX20_CONSTEXPR byte *put(size_t offset, const void *ptr, size_t length) {
      MDBX_INLINE_API_ASSERT(capacity() >= offset + length);
      return static_cast<byte *>(memcpy(get(offset), ptr, length));
    }

    //--------------------------------------------------------------------------

    MDBX_CXX20_CONSTEXPR silo(const allocator_type &alloc = allocator_type()) noexcept : allocator_type(alloc) {}
    MDBX_CXX20_CONSTEXPR silo(size_t capacity, const allocator_type &alloc = allocator_type()) : silo(alloc) {
      if (!bin::is_suitable_for_inplace(capacity))
        bin_.template make_allocated<true>(provide_storage(capacity));
    }

    silo(silo &&other) = delete;
    MDBX_CXX20_CONSTEXPR
    silo(silo &&other, bool is_reference = false) noexcept(::std::is_nothrow_move_constructible<allocator_type>::value)
        : allocator_type(::std::move(other.get_allocator())) {
      if (!is_reference) {
        if (other.bin_.is_inplace()) {
          memcpy(&bin_, &other.bin_, sizeof(bin));
          MDBX_CONSTEXPR_ASSERT(bin_.is_inplace());
        } else {
          /* coverity[USE_AFTER_MOVE] */
          new (&bin_.allocated_ptr_) allocator_pointer(::std::move(other.bin_.allocated_ptr_));
          /* coverity[USE_AFTER_MOVE] */
          bin_.capacity_.bytes_ = other.bin_.capacity_.bytes_;
          /* properly destroy allocator::pointer.
           *
           * CoverityScan issues an erroneous warning here about using an uninitialized object. Which is not true,
           * since in C++ (unlike Rust) an object remains initialized after a move-assignment operation; Moreover,
           * a destructor will be called for such an object (this is explicitly stated in all C++ standards, starting
           * from the 11th). */
          /* coverity[USE_AFTER_MOVE] */
          other.bin_.allocated_ptr_.~allocator_pointer();
          other.bin_.inplace_.lastbyte_ = bin::lastbyte_inplace_signature;
          MDBX_CONSTEXPR_ASSERT(bin_.is_allocated() && other.bin_.is_inplace());
        }
      }
    }

    MDBX_CXX17_CONSTEXPR bool move_content(silo &other, const modality other_modality) noexcept {
      switch (other_modality) {
      case modality::inplace:
        memcpy(&bin_, &other.bin_, sizeof(bin));
        MDBX_CONSTEXPR_ASSERT(bin_.is_inplace());
        return /* buffer's slice fixup is needed */ true;
      case modality::allocated:
        new (&bin_.allocated_ptr_) allocator_pointer(::std::move(other.bin_.allocated_ptr_));
        bin_.capacity_.bytes_ = other.bin_.capacity_.bytes_;
        /* properly destroy allocator::pointer.
         *
         * CoverityScan issues an erroneous warning here about using an uninitialized object. Which is not true,
         * since in C++ (unlike Rust) an object remains initialized after a move-assignment operation; Moreover,
         * a destructor will be called for such an object (this is explicitly stated in all C++ standards, starting
         * from the 11th). */
        /* coverity[use_after_move] */
        other.bin_.allocated_ptr_.~allocator_pointer();
        other.bin_.inplace_.lastbyte_ = bin::lastbyte_inplace_signature;
        MDBX_CONSTEXPR_ASSERT(bin_.is_allocated() && other.bin_.is_inplace());
        return /* buffer's slice fixup is not needed */ false;
      default:
        MDBX_CONSTEXPR_ASSERT(other_modality == modality::reference);
        return /* buffer's slice fixup is not needed */ false;
      }
    }

    static MDBX_CXX20_CONSTEXPR std::pair<bool, bool>
    exchange(silo &left, const modality left_modality, silo &right, const modality right_modality) noexcept(
        allocation_aware_details::swap_alloc<silo, allocator_type>::is_nothrow()) {
      allocation_aware_details::swap_alloc<silo, allocator_type>::propagate(left, right);
      bool left_need_fixup = false, right_need_fixup = false;
      if (left_modality == modality::reference || right_modality == modality::reference) {
        left_need_fixup = left.move_content(right, right_modality);
        right_need_fixup = right.move_content(left, left_modality);
      } else {
        silo temp(std::move(left), false);
        left_need_fixup = left.move_content(right, right_modality);
        right_need_fixup = right.move_content(temp, left_modality);
      }
      return std::make_pair(left_need_fixup, right_need_fixup);
    }

    MDBX_CXX20_CONSTEXPR silo(size_t capacity, size_t headroom, const void *ptr, size_t length,
                              const allocator_type &alloc = allocator_type())
        : silo(capacity, alloc) {
      MDBX_INLINE_API_ASSERT(capacity >= headroom + length);
      if (length)
        put(headroom, ptr, length);
    }

    MDBX_CXX20_CONSTEXPR silo(const void *ptr, size_t length, const allocator_type &alloc = allocator_type())
        : silo(length, 0, ptr, length, alloc) {}

    ~silo() { release(); }

    //--------------------------------------------------------------------------

    MDBX_CXX20_CONSTEXPR void *assign(size_t headroom, const void *ptr, size_t length, size_t tailroom) {
      return reshape<true>(headroom + length + tailroom, headroom, ptr, length);
    }

    MDBX_CXX20_CONSTEXPR void *assign(const void *ptr, size_t length) { return assign(0, ptr, length, 0); }

    MDBX_CXX20_CONSTEXPR void *clear() { return reshape<true>(0, 0, nullptr, 0); }
    MDBX_CXX20_CONSTEXPR void *clear_and_reserve(size_t whole_capacity, size_t headroom) {
      return reshape<false>(whole_capacity, headroom, nullptr, 0);
    }

    MDBX_CXX20_CONSTEXPR void resize(size_t capacity, size_t headroom, slice &content) {
      content.iov_base = reshape<false>(capacity, headroom, content.iov_base, content.iov_len);
    }

    MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX11_CONSTEXPR size_t capacity() const noexcept { return bin_.capacity(); }
    MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX11_CONSTEXPR const void *data(size_t offset = 0) const noexcept {
      return get(offset);
    }
    MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX11_CONSTEXPR void *data(size_t offset = 0) noexcept { return get(offset); }
  };

  MDBX_CXX14_CONSTEXPR void fixup_imported_inplace(const typename silo::bin &src) noexcept {
    auto ptr = inherited::byte_ptr();
    if (src.is_inplace(ptr)) {
      MDBX_CONSTEXPR_ASSERT(silo_.bin_.is_inplace());
      intptr_t offset = &silo_.bin_.inplace_.buffer_[0] - &src.inplace_.buffer_[0];
      iov_base = ptr + offset;
      MDBX_CONSTEXPR_ASSERT(is_freestanding());
    }
  }

  silo silo_;

  void insulate() {
    MDBX_INLINE_API_ASSERT(is_reference());
    iov_base = silo_.assign(iov_base, iov_len);
  }

  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR const byte *silo_begin() const noexcept {
    return static_cast<const byte *>(silo_.data());
  }

  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR const byte *silo_end() const noexcept {
    return silo_begin() + silo_.capacity();
  }

  struct data_preserver : public exception_thunk {
    buffer data;
    data_preserver(allocator_type &alloc) : data(alloc) {}
    static int callback(void *context, MDBX_val *target, const void *src, size_t bytes) noexcept {
      auto self = static_cast<data_preserver *>(context);
      assert(self->is_clean());
      assert(&self->data == target);
      (void)target;
      try {
        self->data.assign(src, bytes, false);
        return MDBX_RESULT_FALSE;
      } catch (... /* capture any exception to rethrow it over C code */) {
        self->capture();
        return MDBX_RESULT_TRUE;
      }
    }
    MDBX_CXX11_CONSTEXPR operator MDBX_preserve_func() const noexcept { return callback; }
    MDBX_CXX11_CONSTEXPR operator const buffer &() const noexcept { return data; }
    MDBX_CXX11_CONSTEXPR operator buffer &() noexcept { return data; }
  };

public:
  /// \todo buffer& operator<<(buffer&, ...) for writing
  /// \todo template<class X> key(X) for encoding keys while writing

  using move_assign_alloc = allocation_aware_details::move_assign_alloc<silo, allocator_type>;
  using copy_assign_alloc = allocation_aware_details::copy_assign_alloc<silo, allocator_type>;
  using swap_alloc = allocation_aware_details::swap_alloc<struct silo, allocator_type>;

  static constexpr bool is_swap_nothrow() noexcept { return swap_alloc::is_nothrow(); }

  /// \brief Returns the associated allocator.
  MDBX_CXX20_CONSTEXPR allocator_type get_allocator() const { return silo_.get_allocator(); }

  /// \brief Checks whether data chunk stored inside the buffer, otherwise
  /// buffer just refers to data located outside the buffer.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR bool is_freestanding() const noexcept {
    static_assert(size_t(-intptr_t(max_length)) > max_length, "WTF?");
    return size_t(inherited::byte_ptr() - silo_begin()) < silo_.capacity();
  }

  /// \brief Checks whether data chunk stored in place within the buffer instance itself,
  /// without reference outside nor allocating additional memory resources,
  /// which also implies buffer is freestanding.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR bool is_inplace() const noexcept {
    return silo_.is_inplace(inherited::data());
  }

  /// \brief Checks whether the buffer just refers to data located outside the buffer, rather than stores it.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR bool is_reference() const noexcept { return !is_freestanding(); }

         /// \brief Returns current data storage modality.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR modality asset() const noexcept {
    return is_freestanding() ? (is_inplace() ? modality::inplace : modality::allocated) : modality::reference;
  }

  /// \brief Returns the number of bytes that can be held in currently allocated storage.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR size_t capacity() const noexcept {
    return is_freestanding() ? silo_.capacity() : 0;
  }

  /// \brief Returns the number of bytes available in the currently allocated storage in front of the data.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR size_t headroom() const noexcept {
    return is_freestanding() ? inherited::byte_ptr() - silo_begin() : 0;
  }

  /// \brief Returns the number of bytes that available in currently allocated storage after the data.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR size_t tailroom() const noexcept {
    return is_freestanding() ? silo_end() - inherited::end_byte_ptr() : 0;
  }

  /// \brief Returns casted to const pointer to byte an address of data.
  MDBX_CXX11_CONSTEXPR const byte *byte_ptr() const noexcept { return inherited::byte_ptr(); }

  /// \brief Returns casted to const pointer to byte an end of data.
  MDBX_CXX11_CONSTEXPR const byte *end_byte_ptr() const noexcept { return inherited::end_byte_ptr(); }

  /// \brief Returns casted to pointer to byte an address of data.
  /// \pre REQUIRES: The buffer should store data chunk, but not referenced to an external one.
  MDBX_CXX11_CONSTEXPR byte *byte_ptr() noexcept {
    MDBX_CONSTEXPR_ASSERT(is_freestanding());
    return inherited::byte_ptr();
  }

  /// \brief Returns casted to pointer to byte an end of data.
  /// \pre REQUIRES: The buffer should store data chunk, but not referenced to an external one.
  MDBX_CXX11_CONSTEXPR byte *end_byte_ptr() noexcept {
    MDBX_CONSTEXPR_ASSERT(is_freestanding());
    return const_cast<byte *>(inherited::end_byte_ptr());
  }

  /// \brief Returns casted to const pointer to char an address of data.
  MDBX_CXX11_CONSTEXPR const char *char_ptr() const noexcept { return inherited::char_ptr(); }

  /// \brief Returns casted to const pointer to char an end of data.
  MDBX_CXX11_CONSTEXPR const char *end_char_ptr() const noexcept { return inherited::end_char_ptr(); }

  /// \brief Returns casted to pointer to char an address of data.
  /// \pre REQUIRES: The buffer should store data chunk, but not referenced to an external one.
  MDBX_CXX11_CONSTEXPR char *char_ptr() noexcept {
    MDBX_CONSTEXPR_ASSERT(is_freestanding());
    return const_cast<char *>(inherited::char_ptr());
  }

  /// \brief Returns casted to pointer to char an end of data.
  /// \pre REQUIRES: The buffer should store data chunk, but not referenced to an external one.
  MDBX_CXX11_CONSTEXPR char *end_char_ptr() noexcept {
    MDBX_CONSTEXPR_ASSERT(is_freestanding());
    return const_cast<char *>(inherited::end_char_ptr());
  }

  /// \brief Return a const pointer to the beginning of the referenced data.
  MDBX_CXX11_CONSTEXPR const void *data() const noexcept { return inherited::data(); }

  /// \brief Return a const pointer to the end of the referenced data.
  MDBX_CXX11_CONSTEXPR const void *end() const noexcept { return inherited::end(); }

  /// \brief Return a pointer to the beginning of the referenced data.
  /// \pre REQUIRES: The buffer should store data chunk, but not referenced to an external one.
  MDBX_CXX11_CONSTEXPR void *data() noexcept {
    MDBX_CONSTEXPR_ASSERT(is_freestanding());
    return const_cast<void *>(inherited::data());
  }

  /// \brief Return a pointer to the end of the referenced data.
  /// \pre REQUIRES: The buffer should store data chunk, but not referenced to an external one.
  MDBX_CXX11_CONSTEXPR void *end() noexcept {
    MDBX_CONSTEXPR_ASSERT(is_freestanding());
    return const_cast<void *>(inherited::end());
  }

  /// \brief Returns the number of bytes.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t length() const noexcept {
    return MDBX_CONSTEXPR_ASSERT(is_reference() || inherited::end_byte_ptr() <= silo_end()), inherited::length();
  }

  /// \brief Set length of data.
  MDBX_CXX14_CONSTEXPR buffer &set_length(size_t bytes) {
    MDBX_CONSTEXPR_ASSERT(is_reference() || inherited::byte_ptr() + bytes <= silo_end());
    inherited::set_length(bytes);
    return *this;
  }

  /// \brief Sets the length by specifying the end of the data.
  MDBX_CXX14_CONSTEXPR buffer &set_end(const void *ptr) {
    MDBX_CONSTEXPR_ASSERT(static_cast<const char *>(ptr) >= char_ptr());
    return set_length(static_cast<const char *>(ptr) - char_ptr());
  }

  /// \brief Makes buffer owning the data.
  /// \details If buffer refers to an external data, then makes it the owner
  /// of clone by allocating storage and copying the data.
  void make_freestanding() {
    if (is_reference())
      insulate();
  }

  MDBX_CXX20_CONSTEXPR buffer() noexcept = default;
  MDBX_CXX20_CONSTEXPR buffer(const allocator_type &alloc) noexcept : silo_(alloc) {}
  MDBX_CXX20_CONSTEXPR buffer(const struct slice &src, bool make_reference,
                              const allocator_type &alloc = allocator_type());

  MDBX_CXX20_CONSTEXPR
  buffer(const void *ptr, size_t bytes, bool make_reference, const allocator_type &alloc = allocator_type())
      : buffer(inherited(ptr, bytes), make_reference, alloc) {}

  template <class CHAR, class T, class A> buffer(const ::std::basic_string<CHAR, T, A> &) = delete;
  template <class CHAR, class T, class A> buffer(const ::std::basic_string<CHAR, T, A> &&) = delete;

  buffer(const char *c_str, bool make_reference, const allocator_type &alloc = allocator_type())
      : buffer(inherited(c_str), make_reference, alloc) {}

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T>
  MDBX_CXX20_CONSTEXPR buffer(const ::std::basic_string_view<CHAR, T> &view, bool make_reference,
                              const allocator_type &alloc = allocator_type())
      : buffer(inherited(view), make_reference, alloc) {}
#endif /* __cpp_lib_string_view >= 201606L */

  MDBX_CXX20_CONSTEXPR
  buffer(const struct slice &src, const allocator_type &alloc = allocator_type())
      : buffer(src, src.empty() || src.is_null(), alloc) {}

  MDBX_CXX20_CONSTEXPR
  buffer(const buffer &src)
      : buffer(src, allocator_traits::select_on_container_copy_construction(src.get_allocator())) {}

  MDBX_CXX20_CONSTEXPR
  buffer(const void *ptr, size_t bytes, const allocator_type &alloc = allocator_type())
      : buffer(inherited(ptr, bytes), alloc) {}

  template <class CHAR, class T, class A>
  MDBX_CXX20_CONSTEXPR buffer(const ::std::basic_string<CHAR, T, A> &str,
                              const allocator_type &alloc = allocator_type())
      : buffer(inherited(str), alloc) {}

  MDBX_CXX20_CONSTEXPR
  buffer(const char *c_str, const allocator_type &alloc = allocator_type()) : buffer(inherited(c_str), alloc) {}

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T>
  MDBX_CXX20_CONSTEXPR buffer(const ::std::basic_string_view<CHAR, T> &view,
                              const allocator_type &alloc = allocator_type())
      : buffer(inherited(view), alloc) {}
#endif /* __cpp_lib_string_view >= 201606L */

  buffer(size_t head_room, size_t tail_room, const allocator_type &alloc = allocator_type())
      : silo_(check_length(head_room, tail_room), alloc) {
    iov_base = silo_.get();
    MDBX_INLINE_API_ASSERT(iov_len == 0);
  }

  buffer(size_t capacity, const allocator_type &alloc = allocator_type()) : silo_(check_length(capacity), alloc) {
    iov_base = silo_.get();
    MDBX_INLINE_API_ASSERT(iov_len == 0);
  }

  buffer(size_t head_room, const slice &src, size_t tail_room, const allocator_type &alloc = allocator_type())
      : silo_(check_length(head_room, src.length(), tail_room), alloc) {
    iov_base = memcpy(silo_.get(), src.data(), iov_len = src.length());
  }

  inline buffer(const txn &transaction, const slice &src, const allocator_type &alloc = allocator_type());

  buffer(buffer &&src) noexcept(noexcept(::std::is_nothrow_move_constructible<allocator_type>::value))
      : inherited(/* no move here */ src), silo_(::std::move(src.silo_), src.is_reference()) {
    /* CoverityScan issues an erroneous warning here about using an uninitialized object. Which is not true,
     * since in C++ (unlike Rust) an object remains initialized after a move-assignment operation; Moreover,
     * a destructor will be called for such an object (this is explicitly stated in all C++ standards, starting from the
     * 11th). */
    /* coverity[USE_AFTER_MOVE] */
    fixup_imported_inplace(src.silo_.bin_);
    src.invalidate();
  }

  /// \brief Build a null buffer which zero length and refers to null address.
  MDBX_CXX14_CONSTEXPR static buffer null() noexcept { return buffer(inherited::null()); }

  /// \brief Build an invalid buffer which non-zero length and refers to null address.
  MDBX_CXX14_CONSTEXPR static buffer invalid() noexcept { return buffer(inherited::invalid()); }

  template <typename POD>
  static buffer wrap(const POD &pod, bool make_reference = false, const allocator_type &alloc = allocator_type()) {
    return buffer(inherited::wrap(pod), make_reference, alloc);
  }

  /// \brief Returns a new buffer with a hexadecimal dump of the slice content.
  static buffer hex(const slice &source, bool uppercase = false, unsigned wrap_width = 0,
                    const allocator_type &alloc = allocator_type()) {
    return source.template encode_hex<ALLOCATOR, CAPACITY_POLICY>(uppercase, wrap_width, alloc);
  }

  /// \brief Returns a new buffer with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the slice content.
  static buffer base58(const slice &source, unsigned wrap_width = 0, const allocator_type &alloc = allocator_type()) {
    return source.template encode_base58<ALLOCATOR, CAPACITY_POLICY>(wrap_width, alloc);
  }
  /// \brief Returns a new buffer with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of the slice content.
  static buffer base64(const slice &source, unsigned wrap_width = 0, const allocator_type &alloc = allocator_type()) {
    return source.template encode_base64<ALLOCATOR, CAPACITY_POLICY>(wrap_width, alloc);
  }

  /// \brief Returns a new buffer with a hexadecimal dump of the given pod.
  template <typename POD>
  static buffer hex(const POD &pod, bool uppercase = false, unsigned wrap_width = 0,
                    const allocator_type &alloc = allocator_type()) {
    return hex(inherited::wrap(pod), uppercase, wrap_width, alloc);
  }

  /// \brief Returns a new buffer with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the given pod.
  template <typename POD>
  static buffer base58(const POD &pod, unsigned wrap_width = 0, const allocator_type &alloc = allocator_type()) {
    return base58(inherited::wrap(pod), wrap_width, alloc);
  }

  /// \brief Returns a new buffer with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of the given pod.
  template <typename POD>
  static buffer base64(const POD &pod, unsigned wrap_width = 0, const allocator_type &alloc = allocator_type()) {
    return base64(inherited::wrap(pod), wrap_width, alloc);
  }

  /// \brief Returns a new buffer with a hexadecimal dump of the slice content.
  buffer encode_hex(bool uppercase = false, unsigned wrap_width = 0,
                    const allocator_type &alloc = allocator_type()) const {
    return inherited::template encode_hex<ALLOCATOR, CAPACITY_POLICY>(uppercase, wrap_width, alloc);
  }

  /// \brief Returns a new buffer with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the slice content.
  buffer encode_base58(unsigned wrap_width = 0, const allocator_type &alloc = allocator_type()) const {
    return inherited::template encode_base58<ALLOCATOR, CAPACITY_POLICY>(wrap_width, alloc);
  }
  /// \brief Returns a new buffer with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of the slice content.
  buffer encode_base64(unsigned wrap_width = 0, const allocator_type &alloc = allocator_type()) const {
    return inherited::template encode_base64<ALLOCATOR, CAPACITY_POLICY>(wrap_width, alloc);
  }

  /// \brief Decodes hexadecimal dump from the slice content to returned buffer.
  static buffer hex_decode(const slice &source, bool ignore_spaces = false,
                           const allocator_type &alloc = allocator_type()) {
    return source.template hex_decode<ALLOCATOR, CAPACITY_POLICY>(ignore_spaces, alloc);
  }

  /// \brief Decodes [Base58](https://en.wikipedia.org/wiki/Base58) dump
  /// from the slice content to returned buffer.
  static buffer base58_decode(const slice &source, bool ignore_spaces = false,
                              const allocator_type &alloc = allocator_type()) {
    return source.template base58_decode<ALLOCATOR, CAPACITY_POLICY>(ignore_spaces, alloc);
  }

  /// \brief Decodes [Base64](https://en.wikipedia.org/wiki/Base64) dump
  /// from the slice content to returned buffer.
  static buffer base64_decode(const slice &source, bool ignore_spaces = false,
                              const allocator_type &alloc = allocator_type()) {
    return source.template base64_decode<ALLOCATOR, CAPACITY_POLICY>(ignore_spaces, alloc);
  }

  /// \brief Decodes hexadecimal dump
  /// from the buffer content to new returned buffer.
  buffer hex_decode(bool ignore_spaces = false, const allocator_type &alloc = allocator_type()) const {
    return hex_decode(*this, ignore_spaces, alloc);
  }

  /// \brief Decodes [Base58](https://en.wikipedia.org/wiki/Base58) dump
  /// from the buffer content to new returned buffer.
  buffer base58_decode(bool ignore_spaces = false, const allocator_type &alloc = allocator_type()) const {
    return base58_decode(*this, ignore_spaces, alloc);
  }

  /// \brief Decodes [Base64](https://en.wikipedia.org/wiki/Base64) dump
  /// from the buffer content to new returned buffer.
  buffer base64_decode(bool ignore_spaces = false, const allocator_type &alloc = allocator_type()) const {
    return base64_decode(*this, ignore_spaces, alloc);
  }

  /// \brief Reserves storage space.
  void reserve(size_t wanna_headroom, size_t wanna_tailroom) {
    wanna_headroom = ::std::min(
        ::std::max(headroom(), wanna_headroom),
        (wanna_headroom < max_length - pettiness_threshold) ? wanna_headroom + pettiness_threshold : wanna_headroom);
    wanna_tailroom = ::std::min(
        ::std::max(tailroom(), wanna_tailroom),
        (wanna_tailroom < max_length - pettiness_threshold) ? wanna_tailroom + pettiness_threshold : wanna_tailroom);
    const size_t wanna_capacity = check_length(wanna_headroom, length(), wanna_tailroom);
    silo_.resize(wanna_capacity, wanna_headroom, *this);
    MDBX_INLINE_API_ASSERT(headroom() >= wanna_headroom && headroom() <= wanna_headroom + pettiness_threshold);
    MDBX_INLINE_API_ASSERT(tailroom() >= wanna_tailroom && tailroom() <= wanna_tailroom + pettiness_threshold);
  }

  /// \brief Reserves space before the payload.
  void reserve_headroom(size_t wanna_headroom) { reserve(wanna_headroom, 0); }

  /// \brief Reserves space after the payload.
  void reserve_tailroom(size_t wanna_tailroom) { reserve(0, wanna_tailroom); }

  buffer &assign_reference(const void *ptr, size_t bytes) {
    silo_.clear();
    inherited::assign(ptr, bytes);
    return *this;
  }

  buffer &assign_freestanding(const void *ptr, size_t bytes) {
    inherited::assign(silo_.assign(static_cast<const typename silo::value_type *>(ptr), check_length(bytes)), bytes);
    return *this;
  }

  MDBX_CXX20_CONSTEXPR void swap(buffer &other) noexcept(swap_alloc::is_nothrow()) {
    const auto pair = silo::exchange(silo_, asset(), other.silo_, other.asset());
    inherited::swap(other);
    if (pair.first)
      fixup_imported_inplace(other.silo_.bin_);
    if (pair.second)
      other.fixup_imported_inplace(silo_.bin_);
  }

  MDBX_CXX20_CONSTEXPR friend void swap(buffer &left, buffer &right) noexcept(buffer::is_swap_nothrow()) {
    left.swap(right);
  }

  static buffer clone(const buffer &src, const allocator_type &alloc = allocator_type()) {
    return buffer(src.headroom(), src, src.tailroom(), alloc);
  }

  MDBX_CXX20_CONSTEXPR buffer make_inplace_or_reference() const {
    return buffer(static_cast<const struct slice &>(*this), !is_inplace(),
                  allocator_traits::select_on_container_copy_construction(get_allocator()));
  }

  MDBX_CXX20_CONSTEXPR buffer &assign(size_t headroom, const buffer &src, size_t tailroom) {
    const size_t whole_capacity = check_length(headroom, src.length(), tailroom);
    if (MDBX_LIKELY(this != &src))
      MDBX_CXX20_LIKELY {
        invalidate();
        if MDBX_IF_CONSTEXPR (!allocation_aware_details::template allocator_is_always_equal<allocator_type>()) {
          if (MDBX_UNLIKELY(silo_.get_allocator() != src.silo_.get_allocator()))
            MDBX_CXX20_UNLIKELY {
              silo_.release();
              allocation_aware_details::copy_assign_alloc<silo, allocator_type>::propagate(&silo_, src.silo_);
            }
        }

        iov_base = silo_.template reshape<true>(whole_capacity, headroom, src.data(), src.length());
        iov_len = src.length();
      }
    else {
      iov_base = silo_.template reshape<false>(whole_capacity, headroom, src.data(), src.length());
    }
    return *this;
  }

  MDBX_CXX20_CONSTEXPR buffer &assign(const buffer &src, bool make_reference = false) {
    if (MDBX_LIKELY(this != &src))
      MDBX_CXX20_LIKELY {
        invalidate();
        if MDBX_IF_CONSTEXPR (!allocation_aware_details::template allocator_is_always_equal<allocator_type>()) {
          if (MDBX_UNLIKELY(silo_.get_allocator() != src.silo_.get_allocator()))
            MDBX_CXX20_UNLIKELY {
              silo_.release();
              allocation_aware_details::copy_assign_alloc<silo, allocator_type>::propagate(&silo_, src.silo_);
            }
        }

        if (make_reference) {
          silo_.release();
          iov_base = src.iov_base;
        } else {
          iov_base = silo_.template reshape<true>(src.length(), 0, src.data(), src.length());
        }
        iov_len = src.length();
      }
    else if (!make_reference && is_reference())
      insulate();
    return *this;
  }

  MDBX_CXX20_CONSTEXPR buffer &assign(buffer &&src) noexcept(move_assign_alloc::is_nothrow()) {
    if (MDBX_LIKELY(this != &src))
      MDBX_CXX20_LIKELY {
        const auto kind = src.asset();
        inherited::assign(std::move(src));
        if (!move_assign_alloc::is_moveable(&silo_, src.silo_) && kind == modality::allocated) {
          iov_base = silo_.template reshape<true>(src.silo_.capacity(), src.headroom(), src.data(), src.length());
          return *this;
        }
        silo_.release();
        move_assign_alloc::propagate(&silo_, src.silo_);
        if (silo_.move_content(src.silo_, kind))
          fixup_imported_inplace(src.silo_.bin_);
      }
    return *this;
  }

  buffer &assign(const void *ptr, size_t bytes, bool make_reference = false) {
    return make_reference ? assign_reference(ptr, bytes) : assign_freestanding(ptr, bytes);
  }

  buffer &assign(const struct slice &src, bool make_reference = false) {
    return assign(src.data(), src.length(), make_reference);
  }

  buffer &assign(const ::MDBX_val &src, bool make_reference = false) {
    return assign(src.iov_base, src.iov_len, make_reference);
  }

  buffer &assign(struct slice &&src, bool make_reference = false) {
    assign(src.data(), src.length(), make_reference);
    src.invalidate();
    return *this;
  }

  buffer &assign(::MDBX_val &&src, bool make_reference = false) {
    assign(src.iov_base, src.iov_len, make_reference);
    src.iov_base = nullptr;
    return *this;
  }

  buffer &assign(const void *begin, const void *end, bool make_reference = false) {
    return assign(begin, static_cast<const byte *>(end) - static_cast<const byte *>(begin), make_reference);
  }

  template <class CHAR, class T, class A>
  buffer &assign(const ::std::basic_string<CHAR, T, A> &str, bool make_reference = false) {
    return assign(str.data(), str.length(), make_reference);
  }

  buffer &assign(const char *c_str, bool make_reference = false) {
    return assign(c_str, strlen(c_str), make_reference);
  }

#if defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L
  template <class CHAR, class T>
  buffer &assign(const ::std::basic_string_view<CHAR, T> &view, bool make_reference = false) {
    return assign(view.data(), view.length(), make_reference);
  }

  template <class CHAR, class T> buffer &assign(::std::basic_string_view<CHAR, T> &&view, bool make_reference = false) {
    assign(view.data(), view.length(), make_reference);
    view = {};
    return *this;
  }
#endif /* __cpp_lib_string_view >= 201606L */

  buffer &operator=(const buffer &src) { return assign(src); }

  buffer &operator=(buffer &&src) noexcept(move_assign_alloc::is_nothrow()) { return assign(::std::move(src)); }

  buffer &operator=(const struct slice &src) { return assign(src); }

  buffer &operator=(struct slice &&src) { return assign(::std::move(src)); }

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T> buffer &operator=(const ::std::basic_string_view<CHAR, T> &view) noexcept {
    return assign(view.begin(), view.length());
  }

  template <class CHAR, class T> buffer &append(const ::std::basic_string_view<CHAR, T> &view) {
    return append(view.data(), view.size());
  }
#endif /* __cpp_lib_string_view >= 201606L */

  template <class CHAR, class T, class A>
  MDBX_CXX20_CONSTEXPR explicit operator ::std::basic_string<CHAR, T, A>() const {
    return as_string<CHAR, T, A>();
  }

  template <class CHAR, class T, class A> buffer &append(const ::std::basic_string<CHAR, T, A> &str) {
    return append(str.data(), str.size());
  }

  /// \brief Clears the contents and storage.
  void clear() noexcept { inherited::assign(silo_.clear(), size_t(0)); }

  /// \brief Clears the contents and reserve storage.
  void clear_and_reserve(size_t whole_capacity, size_t headroom = 0) noexcept {
    inherited::assign(silo_.clear_and_reserve(whole_capacity, headroom), size_t(0));
  }

  /// \brief Reduces memory usage by freeing unused storage space.
  void shrink_to_fit() { silo_.resize(length(), 0, *this); }

  buffer &append(const void *src, size_t bytes) {
    if (MDBX_UNLIKELY(tailroom() < check_length(bytes)))
      MDBX_CXX20_UNLIKELY reserve_tailroom(bytes);
    memcpy(end_byte_ptr(), src, bytes);
    iov_len += bytes;
    return *this;
  }

  buffer &append(const byte c) {
    if (MDBX_UNLIKELY(tailroom() < 1))
      MDBX_CXX20_UNLIKELY reserve_tailroom(1);
    *end_byte_ptr() = c;
    iov_len += 1;
    return *this;
  }

  buffer &append(const struct slice &chunk) { return append(chunk.data(), chunk.size()); }

  buffer &add_header(const void *src, size_t bytes) {
    if (MDBX_UNLIKELY(headroom() < check_length(bytes)))
      MDBX_CXX20_UNLIKELY reserve_headroom(bytes);
    iov_base = memcpy(byte_ptr() - bytes, src, bytes);
    iov_len += bytes;
    return *this;
  }

  buffer &add_header(const struct slice &chunk) { return add_header(chunk.data(), chunk.size()); }

  template <MDBX_CXX20_CONCEPT(MutableByteProducer, PRODUCER)> buffer &append_producer(PRODUCER &producer) {
    const size_t wanna_bytes = producer.envisage_result_length();
    if (MDBX_UNLIKELY(tailroom() < check_length(wanna_bytes)))
      MDBX_CXX20_UNLIKELY reserve_tailroom(wanna_bytes);
    return set_end(producer.write_bytes(end_char_ptr(), tailroom()));
  }

  template <MDBX_CXX20_CONCEPT(ImmutableByteProducer, PRODUCER)> buffer &append_producer(const PRODUCER &producer) {
    const size_t wanna_bytes = producer.envisage_result_length();
    if (MDBX_UNLIKELY(tailroom() < check_length(wanna_bytes)))
      MDBX_CXX20_UNLIKELY reserve_tailroom(wanna_bytes);
    return set_end(producer.write_bytes(end_char_ptr(), tailroom()));
  }

  buffer &append_hex(const struct slice &data, bool uppercase = false, unsigned wrap_width = 0) {
    return append_producer(to_hex(data, uppercase, wrap_width));
  }

  buffer &append_base58(const struct slice &data, unsigned wrap_width = 0) {
    return append_producer(to_base58(data, wrap_width));
  }

  buffer &append_base64(const struct slice &data, unsigned wrap_width = 0) {
    return append_producer(to_base64(data, wrap_width));
  }

  buffer &append_decoded_hex(const struct slice &data, bool ignore_spaces = false) {
    return append_producer(from_hex(data, ignore_spaces));
  }

  buffer &append_decoded_base58(const struct slice &data, bool ignore_spaces = false) {
    return append_producer(from_base58(data, ignore_spaces));
  }

  buffer &append_decoded_base64(const struct slice &data, bool ignore_spaces = false) {
    return append_producer(from_base64(data, ignore_spaces));
  }

  buffer &append_u8(uint_fast8_t u8) {
    if (MDBX_UNLIKELY(tailroom() < 1))
      MDBX_CXX20_UNLIKELY reserve_tailroom(1);
    *end_byte_ptr() = uint8_t(u8);
    iov_len += 1;
    return *this;
  }

  buffer &append_byte(uint_fast8_t byte) { return append_u8(byte); }

  buffer &append_u16(uint_fast16_t u16) {
    if (MDBX_UNLIKELY(tailroom() < 2))
      MDBX_CXX20_UNLIKELY reserve_tailroom(2);
    const auto ptr = end_byte_ptr();
    ptr[0] = uint8_t(u16);
    ptr[1] = uint8_t(u16 >> 8);
    iov_len += 2;
    return *this;
  }

  buffer &append_u24(uint_fast32_t u24) {
    if (MDBX_UNLIKELY(tailroom() < 3))
      MDBX_CXX20_UNLIKELY reserve_tailroom(3);
    const auto ptr = end_byte_ptr();
    ptr[0] = uint8_t(u24);
    ptr[1] = uint8_t(u24 >> 8);
    ptr[2] = uint8_t(u24 >> 16);
    iov_len += 3;
    return *this;
  }

  buffer &append_u32(uint_fast32_t u32) {
    if (MDBX_UNLIKELY(tailroom() < 4))
      MDBX_CXX20_UNLIKELY reserve_tailroom(4);
    const auto ptr = end_byte_ptr();
    ptr[0] = uint8_t(u32);
    ptr[1] = uint8_t(u32 >> 8);
    ptr[2] = uint8_t(u32 >> 16);
    ptr[3] = uint8_t(u32 >> 24);
    iov_len += 4;
    return *this;
  }

  buffer &append_u48(uint_fast64_t u48) {
    if (MDBX_UNLIKELY(tailroom() < 6))
      MDBX_CXX20_UNLIKELY reserve_tailroom(6);
    const auto ptr = end_byte_ptr();
    ptr[0] = uint8_t(u48);
    ptr[1] = uint8_t(u48 >> 8);
    ptr[2] = uint8_t(u48 >> 16);
    ptr[3] = uint8_t(u48 >> 24);
    ptr[4] = uint8_t(u48 >> 32);
    ptr[5] = uint8_t(u48 >> 40);
    iov_len += 6;
    return *this;
  }

  buffer &append_u64(uint_fast64_t u64) {
    if (MDBX_UNLIKELY(tailroom() < 8))
      MDBX_CXX20_UNLIKELY reserve_tailroom(8);
    const auto ptr = end_byte_ptr();
    ptr[0] = uint8_t(u64);
    ptr[1] = uint8_t(u64 >> 8);
    ptr[2] = uint8_t(u64 >> 16);
    ptr[3] = uint8_t(u64 >> 24);
    ptr[4] = uint8_t(u64 >> 32);
    ptr[5] = uint8_t(u64 >> 40);
    ptr[6] = uint8_t(u64 >> 48);
    ptr[7] = uint8_t(u64 >> 56);
    iov_len += 8;
    return *this;
  }

  //----------------------------------------------------------------------------

  template <size_t SIZE> static buffer key_from(const char (&text)[SIZE], bool make_reference = true) {
    return buffer(inherited(text), make_reference);
  }

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T>
  static buffer key_from(const ::std::basic_string_view<CHAR, T> &src, bool make_reference = false) {
    return buffer(src, make_reference);
  }
#endif /* __cpp_lib_string_view >= 201606L */

  static buffer key_from(const char *src, bool make_reference = false) { return buffer(src, make_reference); }

  template <class CHAR, class T, class A>
  static buffer key_from(const ::std::basic_string<CHAR, T, A> &src, bool make_reference = false) {
    return buffer(src, make_reference);
  }

  static buffer key_from(buffer &&src) noexcept { return buffer(::std::move(src)); }

  static buffer key_from_double(const double ieee754_64bit) { return wrap(::mdbx_key_from_double(ieee754_64bit)); }

  static buffer key_from(const double ieee754_64bit) { return key_from_double(ieee754_64bit); }

  static buffer key_from(const double *ieee754_64bit) { return wrap(::mdbx_key_from_ptrdouble(ieee754_64bit)); }

  static buffer key_from_u64(const uint64_t unsigned_int64) { return wrap(unsigned_int64); }

  static buffer key_from(const uint64_t unsigned_int64) { return key_from_u64(unsigned_int64); }

  static buffer key_from_i64(const int64_t signed_int64) { return wrap(::mdbx_key_from_int64(signed_int64)); }

  static buffer key_from(const int64_t signed_int64) { return key_from_i64(signed_int64); }

  static buffer key_from_jsonInteger(const int64_t json_integer) {
    return wrap(::mdbx_key_from_jsonInteger(json_integer));
  }

  static buffer key_from_float(const float ieee754_32bit) { return wrap(::mdbx_key_from_float(ieee754_32bit)); }

  static buffer key_from(const float ieee754_32bit) { return key_from_float(ieee754_32bit); }

  static buffer key_from(const float *ieee754_32bit) { return wrap(::mdbx_key_from_ptrfloat(ieee754_32bit)); }

  static buffer key_from_u32(const uint32_t unsigned_int32) { return wrap(unsigned_int32); }

  static buffer key_from(const uint32_t unsigned_int32) { return key_from_u32(unsigned_int32); }

  static buffer key_from_i32(const int32_t signed_int32) { return wrap(::mdbx_key_from_int32(signed_int32)); }

  static buffer key_from(const int32_t signed_int32) { return key_from_i32(signed_int32); }

  const slice &slice() const noexcept { return *this; }
};

template <typename ALLOCATOR, typename CAPACITY_POLICY> struct buffer_pair_spec {
  using buffer_type = buffer<ALLOCATOR, CAPACITY_POLICY>;
  using allocator_type = typename buffer_type::allocator_type;
  using allocator_traits = typename buffer_type::allocator_traits;
  using reservation_policy = CAPACITY_POLICY;
  using stl_pair = ::std::pair<buffer_type, buffer_type>;
  buffer_type key, value;

  MDBX_CXX20_CONSTEXPR buffer_pair_spec() noexcept = default;
  MDBX_CXX20_CONSTEXPR
  buffer_pair_spec(const allocator_type &alloc) noexcept : key(alloc), value(alloc) {}

  buffer_pair_spec(const buffer_type &key, const buffer_type &value, const allocator_type &alloc = allocator_type())
      : key(key, alloc), value(value, alloc) {}
  buffer_pair_spec(const buffer_type &key, const buffer_type &value, bool make_reference,
                   const allocator_type &alloc = allocator_type())
      : key(key, make_reference, alloc), value(value, make_reference, alloc) {}

  buffer_pair_spec(const stl_pair &pair, const allocator_type &alloc = allocator_type())
      : buffer_pair_spec(pair.first, pair.second, alloc) {}
  buffer_pair_spec(const stl_pair &pair, bool make_reference, const allocator_type &alloc = allocator_type())
      : buffer_pair_spec(pair.first, pair.second, make_reference, alloc) {}

  buffer_pair_spec(const slice &key, const slice &value, const allocator_type &alloc = allocator_type())
      : key(key, alloc), value(value, alloc) {}
  buffer_pair_spec(const slice &key, const slice &value, bool make_reference,
                   const allocator_type &alloc = allocator_type())
      : key(key, make_reference, alloc), value(value, make_reference, alloc) {}

  buffer_pair_spec(const pair &pair, const allocator_type &alloc = allocator_type())
      : buffer_pair_spec(pair.key, pair.value, alloc) {}
  buffer_pair_spec(const pair &pair, bool make_reference, const allocator_type &alloc = allocator_type())
      : buffer_pair_spec(pair.key, pair.value, make_reference, alloc) {}

  buffer_pair_spec(const txn &transaction, const slice &key, const slice &value,
                   const allocator_type &alloc = allocator_type())
      : key(transaction, key, alloc), value(transaction, value, alloc) {}
  buffer_pair_spec(const txn &transaction, const pair &pair, const allocator_type &alloc = allocator_type())
      : buffer_pair_spec(transaction, pair.key, pair.value, alloc) {}

  buffer_pair_spec(buffer_type &&key, buffer_type &&value) noexcept(buffer_type::move_assign_alloc::is_nothrow())
      : key(::std::move(key)), value(::std::move(value)) {}
  buffer_pair_spec(const buffer_pair_spec &) = default;
  buffer_pair_spec(buffer_pair_spec &&pair) noexcept(buffer_type::move_assign_alloc::is_nothrow())
      : buffer_pair_spec(::std::move(pair.key), ::std::move(pair.value)) {}

  buffer_pair_spec &operator=(const buffer_pair_spec &) = default;
  buffer_pair_spec &operator=(buffer_pair_spec &&src) {
    key.assign(std::move(src.key));
    value.assign(std::move(src.value));
    return *this;
  }

  buffer_pair_spec &operator=(const pair &src) {
    key.assign(src.key);
    value.assign(src.value);
    return *this;
  }
  buffer_pair_spec &operator=(pair &&src) {
    key.assign(std::move(src.key));
    value.assign(std::move(src.value));
    return *this;
  }
  buffer_pair_spec &operator=(const stl_pair &src) {
    key.assign(src.first);
    value.assign(src.second);
    return *this;
  }
  buffer_pair_spec &operator=(stl_pair &&src) {
    key.assign(std::move(src.first));
    value.assign(std::move(src.second));
    return *this;
  }

  /// \brief Checks whether data chunk stored inside the buffers both, otherwise
  /// at least one of buffers just refers to data located outside.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR bool is_freestanding() const noexcept {
    return key.is_freestanding() && value.is_freestanding();
  }
  /// \brief Checks whether one of the buffers just refers to data located
  /// outside the buffer, rather than stores it.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX20_CONSTEXPR bool is_reference() const noexcept {
    return key.is_reference() || value.is_reference();
  }
  /// \brief Makes buffers owning the data.
  /// \details If buffer refers to an external data, then makes it the owner
  /// of clone by allocating storage and copying the data.
  void make_freestanding() {
    key.make_freestanding();
    value.make_freestanding();
  }

  operator pair() const noexcept { return pair(key, value); }
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
