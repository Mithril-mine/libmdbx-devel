// > dist-cutoff-begin
#pragma once
#include "decl_buffer.h++"
#include "decl_core.h++"
#include "decl_env.h++"
#include "decl_exceptions.h++"
#include "decl_txn.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief Unmanaged cursor.
///
/// Like other unmanaged classes, `cursor` allows copying and assignment for handles as a values,
/// but does not manage nor destroys the represented underlying object from the own class destructor.
///
/// \copydetails MDBX_cursor
class LIBMDBX_API_TYPE cursor {
protected:
  MDBX_cursor *handle_{nullptr};

public:
  MDBX_CXX11_CONSTEXPR cursor(MDBX_cursor *ptr) noexcept;
  MDBX_CXX11_CONSTEXPR cursor() noexcept = default;
  cursor(const cursor &) noexcept = default;
  cursor &operator=(const cursor &) noexcept = default;
  inline cursor &operator=(cursor &&) noexcept;
  inline cursor(cursor &&) noexcept;
  inline ~cursor() noexcept;
  cursor_managed clone(void *your_context = nullptr) const;
  inline cursor &assign(const cursor &);
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept { return handle_ != nullptr; };
  MDBX_CXX14_CONSTEXPR operator const MDBX_cursor *() const noexcept { return handle_; }
  MDBX_CXX14_CONSTEXPR operator MDBX_cursor *() noexcept { return handle_; }
  MDBX_CXX14_CONSTEXPR const MDBX_cursor *handle() const noexcept { return handle_; }
  MDBX_CXX14_CONSTEXPR MDBX_cursor *handle() noexcept { return handle_; };
  friend MDBX_CXX11_CONSTEXPR bool operator==(const cursor &a, const cursor &b) noexcept {
    return a.handle_ == b.handle_;
  }
  friend MDBX_CXX11_CONSTEXPR bool operator!=(const cursor &a, const cursor &b) noexcept {
    return a.handle_ != b.handle_;
  }

  friend inline int compare_position_nothrow(const cursor &left, const cursor &right, bool ignore_nested) noexcept;
  friend inline int compare_position(const cursor &left, const cursor &right, bool ignore_nested);

  bool is_before_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) < 0;
  }

  bool is_same_or_before_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) <= 0;
  }

  bool is_same_position(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) == 0;
  }

  bool is_after_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) > 0;
  }

  bool is_same_or_after_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) >= 0;
  }

  /// \brief Returns the application context associated with the cursor.
  inline void *get_context() const noexcept;

  /// \brief Sets the application context associated with the cursor.
  inline cursor &set_context(void *your_context);

  enum move_operation {
    first = MDBX_FIRST,
    last = MDBX_LAST,
    next = MDBX_NEXT,
    previous = MDBX_PREV,
    get_current = MDBX_GET_CURRENT,

    multi_prevkey_lastvalue = MDBX_PREV_NODUP,
    multi_currentkey_firstvalue = MDBX_FIRST_DUP,
    multi_currentkey_prevvalue = MDBX_PREV_DUP,
    multi_currentkey_nextvalue = MDBX_NEXT_DUP,
    multi_currentkey_lastvalue = MDBX_LAST_DUP,
    multi_nextkey_firstvalue = MDBX_NEXT_NODUP,

    multi_find_pair = MDBX_GET_BOTH,
    multi_exactkey_lowerboundvalue = MDBX_GET_BOTH_RANGE,

    seek_key = MDBX_SET,
    key_exact = MDBX_SET_KEY,
    key_lowerbound = MDBX_SET_RANGE,

    /* Doubtless cursor positioning at a specified key. */
    key_lesser_than = MDBX_TO_KEY_LESSER_THAN,
    key_lesser_or_equal = MDBX_TO_KEY_LESSER_OR_EQUAL,
    key_equal = MDBX_TO_KEY_EQUAL,
    key_greater_or_equal = MDBX_TO_KEY_GREATER_OR_EQUAL,
    key_greater_than = MDBX_TO_KEY_GREATER_THAN,

    /* Doubtless cursor positioning at a specified key-value pair
     * for dupsort/multi-value hives. */
    multi_exactkey_value_lesser_than = MDBX_TO_EXACT_KEY_VALUE_LESSER_THAN,
    multi_exactkey_value_lesser_or_equal = MDBX_TO_EXACT_KEY_VALUE_LESSER_OR_EQUAL,
    multi_exactkey_value_equal = MDBX_TO_EXACT_KEY_VALUE_EQUAL,
    multi_exactkey_value_greater_or_equal = MDBX_TO_EXACT_KEY_VALUE_GREATER_OR_EQUAL,
    multi_exactkey_value_greater = MDBX_TO_EXACT_KEY_VALUE_GREATER_THAN,

    pair_lesser_than = MDBX_TO_PAIR_LESSER_THAN,
    pair_lesser_or_equal = MDBX_TO_PAIR_LESSER_OR_EQUAL,
    pair_equal = MDBX_TO_PAIR_EQUAL,
    pair_exact = pair_equal,
    pair_greater_or_equal = MDBX_TO_PAIR_GREATER_OR_EQUAL,
    pair_greater_than = MDBX_TO_PAIR_GREATER_THAN,

    batch_samelength = MDBX_GET_MULTIPLE,
    batch_samelength_next = MDBX_NEXT_MULTIPLE,
    batch_samelength_previous = MDBX_PREV_MULTIPLE,
    seek_and_batch_samelength = MDBX_SEEK_AND_GET_MULTIPLE
  };

  // TODO: добавить легковесный proxy-класс для замещения параметра throw_notfound более сложным набором опций,
  //       в том числе с explicit-конструктором из bool, чтобы защититься от неявной конвертации ключей поиска
  //       и других параметров в bool-throw_notfound.

  struct move_result : public pair_result {
    inline move_result(const cursor &cursor, bool throw_notfound);
    move_result(cursor &cursor, move_operation operation, bool throw_notfound)
        : move_result(cursor, operation, slice::invalid(), slice::invalid(), throw_notfound) {}
    move_result(cursor &cursor, move_operation operation, const slice &key, bool throw_notfound)
        : move_result(cursor, operation, key, slice::invalid(), throw_notfound) {}
    inline move_result(cursor &cursor, move_operation operation, const slice &key, const slice &value,
                       bool throw_notfound);
    move_result(const move_result &) noexcept = default;
    move_result &operator=(const move_result &) noexcept = default;
  };

  struct estimate_result : public pair {
    ptrdiff_t approximate_quantity;
    estimate_result(const cursor &cursor, move_operation operation)
        : estimate_result(cursor, operation, slice::invalid(), slice::invalid()) {}
    estimate_result(const cursor &cursor, move_operation operation, const slice &key)
        : estimate_result(cursor, operation, key, slice::invalid()) {}
    inline estimate_result(const cursor &cursor, move_operation operation, const slice &key, const slice &value);
    estimate_result(const estimate_result &) noexcept = default;
    estimate_result &operator=(const estimate_result &) noexcept = default;
  };

protected:
  /* fake `const`, but for specific move/get operations */
  inline bool move(move_operation operation, MDBX_val *key, MDBX_val *value, bool throw_notfound) const;

  inline ptrdiff_t estimate(move_operation operation, MDBX_val *key, MDBX_val *value) const;

public:
  template <typename CALLABLE_PREDICATE>
  bool scan_until(CALLABLE_PREDICATE predicate, move_operation start = first, move_operation turn = next) {
    struct wrapper : public exception_thunk {
      static int probe(void *context, MDBX_val *key, MDBX_val *value, void *arg) noexcept {
        auto thunk = static_cast<wrapper *>(context);
        assert(thunk->is_clean());
        auto &predicate = *static_cast<CALLABLE_PREDICATE *>(arg);
        try {
          return predicate(pair(*key, *value)) ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
        } catch (... /* capture any exception to rethrow it over C code */) {
          thunk->capture();
          return MDBX_RESULT_TRUE;
        }
      }
    } thunk;
    return error::boolean_or_throw(
        ::mdbx_cursor_scan(handle_, wrapper::probe, &thunk, MDBX_cursor_op(start), MDBX_cursor_op(turn), &predicate),
        thunk);
  }

  template <typename CALLABLE_PREDICATE> bool fullscan(CALLABLE_PREDICATE predicate, bool backward = false) {
    return scan_until(std::move(predicate), backward ? last : first, backward ? previous : next);
  }

  template <typename CALLABLE_PREDICATE>
  bool scan_until_from(CALLABLE_PREDICATE predicate, slice &from, move_operation start = key_greater_or_equal,
                       move_operation turn = next) {
    struct wrapper : public exception_thunk {
      static int probe(void *context, MDBX_val *key, MDBX_val *value, void *arg) noexcept {
        auto thunk = static_cast<wrapper *>(context);
        assert(thunk->is_clean());
        auto &predicate = *static_cast<CALLABLE_PREDICATE *>(arg);
        try {
          return predicate(pair(*key, *value)) ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
        } catch (... /* capture any exception to rethrow it over C code */) {
          thunk->capture();
          return MDBX_RESULT_TRUE;
        }
      }
    } thunk;
    return error::boolean_or_throw(::mdbx_cursor_scan_from(handle_, wrapper::probe, &thunk, MDBX_cursor_op(start),
                                                           &from, nullptr, MDBX_cursor_op(turn), &predicate),
                                   thunk);
  }

  template <typename CALLABLE_PREDICATE>
  bool scan_until_from(CALLABLE_PREDICATE predicate, pair &from, move_operation start = pair_greater_or_equal,
                       move_operation turn = next) {
    struct wrapper : public exception_thunk {
      static int probe(void *context, MDBX_val *key, MDBX_val *value, void *arg) noexcept {
        auto thunk = static_cast<wrapper *>(context);
        assert(thunk->is_clean());
        auto &predicate = *static_cast<CALLABLE_PREDICATE *>(arg);
        try {
          return predicate(pair(*key, *value)) ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
        } catch (... /* capture any exception to rethrow it over C code */) {
          thunk->capture();
          return MDBX_RESULT_TRUE;
        }
      }
    } thunk;
    return error::boolean_or_throw(::mdbx_cursor_scan_from(handle_, wrapper::probe, &thunk, MDBX_cursor_op(start),
                                                           &from.key, &from.value, MDBX_cursor_op(turn), &predicate),
                                   thunk);
  }

  move_result move(move_operation operation, bool throw_notfound) {
    return move_result(*this, operation, throw_notfound);
  }
  move_result move(move_operation operation, const slice &key, bool throw_notfound) {
    return move_result(*this, operation, key, slice::invalid(), throw_notfound);
  }
  move_result move(move_operation operation, const slice &key, const slice &value, bool throw_notfound) {
    return move_result(*this, operation, key, value, throw_notfound);
  }
  bool move(move_operation operation, slice &key, slice &value, bool throw_notfound) {
    return move(operation, &key, &value, throw_notfound);
  }

  move_result to_first(bool throw_notfound = true) { return move(first, throw_notfound); }
  move_result to_previous(bool throw_notfound = true) { return move(previous, throw_notfound); }
  move_result to_previous_last_multi(bool throw_notfound = true) {
    return move(multi_prevkey_lastvalue, throw_notfound);
  }
  move_result to_current_first_multi(bool throw_notfound = true) {
    return move(multi_currentkey_firstvalue, throw_notfound);
  }
  move_result to_current_prev_multi(bool throw_notfound = true) {
    return move(multi_currentkey_prevvalue, throw_notfound);
  }
  move_result current(bool throw_notfound = true) const { return move_result(*this, throw_notfound); }
  move_result to_current_next_multi(bool throw_notfound = true) {
    return move(multi_currentkey_nextvalue, throw_notfound);
  }
  move_result to_current_last_multi(bool throw_notfound = true) {
    return move(multi_currentkey_lastvalue, throw_notfound);
  }
  move_result to_next_first_multi(bool throw_notfound = true) { return move(multi_nextkey_firstvalue, throw_notfound); }
  move_result to_next(bool throw_notfound = true) { return move(next, throw_notfound); }
  move_result to_last(bool throw_notfound = true) { return move(last, throw_notfound); }

  move_result to_key_lesser_than(const slice &key, bool throw_notfound = true) {
    return move(key_lesser_than, key, throw_notfound);
  }
  move_result to_key_lesser_or_equal(const slice &key, bool throw_notfound = true) {
    return move(key_lesser_or_equal, key, throw_notfound);
  }
  move_result to_key_equal(const slice &key, bool throw_notfound = true) {
    return move(key_equal, key, throw_notfound);
  }
  move_result to_key_exact(const slice &key, bool throw_notfound = true) {
    return move(key_exact, key, throw_notfound);
  }
  move_result to_key_greater_or_equal(const slice &key, bool throw_notfound = true) {
    return move(key_greater_or_equal, key, throw_notfound);
  }
  move_result to_key_greater_than(const slice &key, bool throw_notfound = true) {
    return move(key_greater_than, key, throw_notfound);
  }

  move_result to_exact_key_value_lesser_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_lesser_than, key, value, throw_notfound);
  }
  move_result to_exact_key_value_lesser_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_lesser_or_equal, key, value, throw_notfound);
  }
  move_result to_exact_key_value_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_equal, key, value, throw_notfound);
  }
  move_result to_exact_key_value_greater_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_greater_or_equal, key, value, throw_notfound);
  }
  move_result to_exact_key_value_greater_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_greater, key, value, throw_notfound);
  }

  move_result to_pair_lesser_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_lesser_than, key, value, throw_notfound);
  }
  move_result to_pair_lesser_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_lesser_or_equal, key, value, throw_notfound);
  }
  move_result to_pair_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_equal, key, value, throw_notfound);
  }
  move_result to_pair_exact(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_exact, key, value, throw_notfound);
  }
  move_result to_pair_greater_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_greater_or_equal, key, value, throw_notfound);
  }
  move_result to_pair_greater_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_greater_than, key, value, throw_notfound);
  }

  inline bool seek(const slice &key);
  inline move_result find(const slice &key, bool throw_notfound = true);
  inline move_result lower_bound(const slice &key, bool throw_notfound = false);
  inline move_result upper_bound(const slice &key, bool throw_notfound = false);

  /// \brief Return count of duplicates for current key.
  inline size_t count_multivalue() const;

  inline move_result find_multivalue(const slice &key, const slice &value, bool throw_notfound = true);
  inline move_result lower_bound_multivalue(const slice &key, const slice &value, bool throw_notfound = false);
  inline move_result upper_bound_multivalue(const slice &key, const slice &value, bool throw_notfound = false);

  inline move_result seek_multiple_samelength(const slice &key, bool throw_notfound = true) {
    return move(seek_and_batch_samelength, key, throw_notfound);
  }

  inline move_result get_multiple_samelength(bool throw_notfound = false) {
    return move(batch_samelength, throw_notfound);
  }

  inline move_result next_multiple_samelength(bool throw_notfound = false) {
    return move(batch_samelength_next, throw_notfound);
  }

  inline move_result previous_multiple_samelength(bool throw_notfound = false) {
    return move(batch_samelength_previous, throw_notfound);
  }

  inline bool eof() const;
  inline bool on_first() const;
  inline bool on_last() const;
  inline bool on_first_multival() const;
  inline bool on_last_multival() const;
  inline estimate_result estimate(const slice &key, const slice &value) const;
  inline estimate_result estimate(const slice &key) const;
  inline estimate_result estimate(move_operation operation) const;
  inline estimate_result estimate(move_operation operation, slice &key) const;

  static inline ptrdiff_t distance_between(const cursor from, const cursor to,
                                           unsigned deepness = /* enough to cover whole tree height */ 42);
  inline ptrdiff_t distance_from(const cursor from,
                                 unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(from, *this, deepness);
  }
  inline ptrdiff_t distance_to(const cursor to, unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(*this, to, deepness);
  }
  inline ptrdiff_t distance_from_first(unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(nullptr, *this, deepness);
  }
  inline ptrdiff_t distance_to_end(unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(*this, nullptr, deepness);
  }

  inline bool scroll(intptr_t distable, unsigned deepness = /* enough to cover whole tree height */ 42,
                     bool throw_notfound = true);

  static inline bool distribute(const cursor from, const cursor to, cursor *cursors_array, intptr_t cursors_array_size,
                                unsigned deepness = /* enough to cover whole tree height */ 42);

  static inline bool distribute(const cursor from, const cursor to, const std::vector<cursor> &cursors,
                                unsigned deepness = /* enough to cover whole tree height */ 42);

  static inline bool distribute(const cursor from, const cursor to, const std::vector<cursor_managed> &cursors,
                                unsigned deepness = /* enough to cover whole tree height */ 42);

  //----------------------------------------------------------------------------

  /// \brief Renew/bind a cursor with a new transaction and previously used key-value map handle.
  inline void renew(::mdbx::txn &txn);

  /// \brief Bind/renew a cursor with a new transaction and specified key-value map handle.
  inline void bind(::mdbx::txn &txn, ::mdbx::map_handle map_handle);

  /// \brief Unbind cursor from a transaction.
  inline void unbind();

  /// \brief Returns the cursor's transaction.
  inline ::mdbx::txn txn() const;
  inline map_handle map() const;

  inline operator ::mdbx::txn() const { return txn(); }
  inline operator ::mdbx::map_handle() const { return map(); }

  inline MDBX_error_t put(const slice &key, slice *value, MDBX_put_flags_t flags) noexcept;
  inline void put(const slice &key, slice value, put_mode mode);
  inline void insert(const slice &key, slice value);
  inline value_result try_insert(const slice &key, slice value);
  inline slice insert_reserve(const slice &key, size_t value_length);
  inline value_result try_insert_reserve(const slice &key, size_t value_length);

  inline void upsert(const slice &key, const slice &value);
  inline slice upsert_reserve(const slice &key, size_t value_length);

  /// \brief Updates value associated with a key at the current cursor position.
  void update_current(const slice &value);
  /// \brief Reserves and returns the space to storing a value associated with a key at the current cursor position.
  slice reverse_current(size_t value_length);

  inline void update(const slice &key, const slice &value);
  inline bool try_update(const slice &key, const slice &value);
  inline slice update_reserve(const slice &key, size_t value_length);
  inline value_result try_update_reserve(const slice &key, size_t value_length);

  void put(const pair &kv, put_mode mode) { return put(kv.key, kv.value, mode); }
  void insert(const pair &kv) { return insert(kv.key, kv.value); }
  value_result try_insert(const pair &kv) { return try_insert(kv.key, kv.value); }
  void upsert(const pair &kv) { return upsert(kv.key, kv.value); }

  /// \brief Removes single key-value pair or all multi-values at the current cursor position.
  inline bool erase(bool whole_multivalue = false);

  /// \brief Seeks and removes first value or whole multi-value of the given key.
  /// \return `True` if the key is found and a value(s) is removed.
  inline bool erase(const slice &key, bool whole_multivalue = true);

  /// \brief Seeks and removes the particular multi-value entry of the key.
  /// \return `True` if the given key-value pair is found and removed.
  inline bool erase(const slice &key, const slice &value);

  inline size_t put_multiple_samelength(const slice &key, const size_t value_length, const void *values_array,
                                        size_t values_count, put_mode mode, bool allow_partial = false);
  template <typename VALUE>
  size_t put_multiple_samelength(const slice &key, const VALUE *values_array, size_t values_count, put_mode mode,
                                 bool allow_partial = false) {
    static_assert(::std::is_standard_layout<VALUE>::value && !::std::is_pointer<VALUE>::value &&
                      !::std::is_array<VALUE>::value,
                  "Must be a standard layout type!");
    return put_multiple_samelength(key, sizeof(VALUE), values_array, values_count, mode, allow_partial);
  }
  template <typename VALUE>
  void put_multiple_samelength(const slice &key, const ::std::vector<VALUE> &vector, put_mode mode) {
    put_multiple_samelength(key, vector.data(), vector.size(), mode);
  }
};

/// \brief Managed cursor.
///
/// As other managed classes, `cursor_managed` destroys the represented
/// underlying object from the own class destructor, but disallows copying and
/// assignment for instances.
///
/// \copydetails MDBX_cursor
class LIBMDBX_API_TYPE cursor_managed : public cursor {
  using inherited = cursor;
  friend class txn;
  /// delegated constructor for RAII
  MDBX_CXX11_CONSTEXPR cursor_managed(MDBX_cursor *ptr) noexcept : inherited(ptr) {}

public:
  /// \brief Creates a new managed cursor with underlying object.
  cursor_managed(void *your_context = nullptr) : cursor_managed(::mdbx_cursor_create(your_context)) {
    if (MDBX_UNLIKELY(!handle_))
      MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_ENOMEM);
  }

  /// \brief Explicitly closes the cursor.
  void close();

  cursor_managed(cursor_managed &&) = default;
  cursor_managed &operator=(cursor_managed &&other) noexcept;

  inline MDBX_cursor *withdraw_handle() noexcept {
    MDBX_cursor *handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  cursor_managed(const cursor_managed &) = delete;
  cursor_managed &operator=(const cursor_managed &) = delete;
  ~cursor_managed();
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
