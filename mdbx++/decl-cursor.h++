// > dist-cutoff-begin
#pragma once
#include "decl-buffer.h++"
#include "decl-core.h++"
#include "decl-env.h++"
#include "decl-exceptions.h++"
#include "decl-txn.h++"
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
  MDBX_cursor *handle_{nullptr}; ///< \brief The underlying C cursor handle, null for an empty instance.

public:
  /// \brief Constructs a non-owning cursor wrapping the given C handle.
  /// \param ptr  The pointer to the underlying \ref MDBX_cursor object.
  MDBX_CXX11_CONSTEXPR cursor(MDBX_cursor *ptr) noexcept;
  /// \brief Constructs an empty (null) cursor.
  MDBX_CXX11_CONSTEXPR cursor() noexcept = default;
  /// \brief Copy constructor.
  cursor(const cursor &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  cursor &operator=(const cursor &) noexcept = default;
  /// \brief Move assignment operator.
  /// \param other  The source cursor to be moved from.
  /// \returns A reference to this instance.
  inline cursor &operator=(cursor &&other) noexcept;
  /// \brief Move constructor: the source cursor is nullified.
  /// \param other  The source cursor to be moved from.
  inline cursor(cursor &&other) noexcept;
  /// \brief Destroys the wrapper; the underlying object is NOT closed
  /// since the cursor is non-owning.
  inline ~cursor() noexcept;
  /* Slicing an owning cursor_managed temporary into the base handle would drop
   * ownership and leak the cursor (issue #77), so these exact-match overloads
   * turn `mdbx::cursor c = txn.open_cursor(map);` into a compile error, while
   * a non-owning view of a cursor_managed lvalue is still allowed. */
  cursor(cursor_managed &&) = delete;
  cursor &operator=(cursor_managed &&) = delete;
  /// \brief Creates a new managed cursor with the same position and context.
  /// \param your_context  An application context to be associated with the clone.
  /// \returns The new \ref cursor_managed instance.
  /// \throws mdbx::error on failure.
  cursor_managed clone(void *your_context = nullptr) const;
  /// \brief Assigns this cursor to refer to the same handle as the source.
  /// \param src  The source cursor.
  /// \returns A reference to this instance.
  inline cursor &assign(const cursor &);
  /// \brief Checks whether the cursor wraps a non-null handle.
  /// \returns `true` if the cursor is not null.
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept { return handle_ != nullptr; };
  /// \brief Returns the underlying C handle (read-only).
  /// \returns The constant pointer to the \ref MDBX_cursor object.
  MDBX_CXX14_CONSTEXPR operator const MDBX_cursor *() const noexcept { return handle_; }
  /// \brief Returns the underlying C handle.
  /// \returns The mutable pointer to the \ref MDBX_cursor object.
  MDBX_CXX14_CONSTEXPR operator MDBX_cursor *() noexcept { return handle_; }
  /// \brief Returns the underlying C handle (read-only).
  /// \returns The constant pointer to the \ref MDBX_cursor object.
  MDBX_CXX14_CONSTEXPR const MDBX_cursor *handle() const noexcept { return handle_; }
  /// \brief Returns the underlying C handle.
  /// \returns The mutable pointer to the \ref MDBX_cursor object.
  MDBX_CXX14_CONSTEXPR MDBX_cursor *handle() noexcept { return handle_; };
  friend MDBX_CXX11_CONSTEXPR bool operator==(const cursor &a, const cursor &b) noexcept {
    return a.handle_ == b.handle_;
  }
  friend MDBX_CXX11_CONSTEXPR bool operator!=(const cursor &a, const cursor &b) noexcept {
    return a.handle_ != b.handle_;
  }

  friend inline int compare_position_nothrow(const cursor &left, const cursor &right, bool ignore_nested) noexcept;
  friend inline int compare_position(const cursor &left, const cursor &right, bool ignore_nested);

  /// \brief Checks whether this cursor is before the given one.
  /// \param other          The cursor to compare the position with.
  /// \param ignore_nested  Ignore the position within a nested (dupsort) tree.
  /// \returns `true` if this cursor position is less than the other one.
  /// \throws `std::logic_error` if the cursors are incomparable.
  bool is_before_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) < 0;
  }

  /// \brief Checks whether this cursor is at the same position or before the given one.
  /// \param other          The cursor to compare the position with.
  /// \param ignore_nested  Ignore the position within a nested (dupsort) tree.
  /// \returns `true` if this cursor position is less than or equal to the other one.
  /// \throws `std::logic_error` if the cursors are incomparable.
  bool is_same_or_before_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) <= 0;
  }

  /// \brief Checks whether this cursor is at the same position as the given one.
  /// \param other          The cursor to compare the position with.
  /// \param ignore_nested  Ignore the position within a nested (dupsort) tree.
  /// \returns `true` if this cursor position is equal to the other one.
  /// \throws `std::logic_error` if the cursors are incomparable.
  bool is_same_position(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) == 0;
  }

  /// \brief Checks whether this cursor is after the given one.
  /// \param other          The cursor to compare the position with.
  /// \param ignore_nested  Ignore the position within a nested (dupsort) tree.
  /// \returns `true` if this cursor position is greater than the other one.
  /// \throws `std::logic_error` if the cursors are incomparable.
  bool is_after_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) > 0;
  }

  /// \brief Checks whether this cursor is at the same position or after the given one.
  /// \param other          The cursor to compare the position with.
  /// \param ignore_nested  Ignore the position within a nested (dupsort) tree.
  /// \returns `true` if this cursor position is greater than or equal to the other one.
  /// \throws `std::logic_error` if the cursors are incomparable.
  bool is_same_or_after_than(const cursor &other, bool ignore_nested = false) const {
    return compare_position(*this, other, ignore_nested) >= 0;
  }

  /// \brief Returns the application context associated with the cursor.
  /// \returns The application context pointer.
  inline void *get_context() const noexcept;

  /// \brief Sets the application context associated with the cursor.
  /// \param your_context  The application context pointer.
  /// \returns A reference to this instance.
  inline cursor &set_context(void *your_context);

  /// \brief Cursor positioning operations, mirrors \ref MDBX_cursor_op.
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

  /// \brief The result of a cursor move/seek operation: the found key-value
  /// pair plus the success flag.
  struct move_result : public pair_result {
    /// \brief Reads the current position of the given cursor.
    /// \param cursor         The cursor to read the position from.
    /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
    inline move_result(const cursor &cursor, bool throw_notfound);
    /// \brief Performs the given positioning operation.
    /// \param cursor         The cursor to be moved.
    /// \param operation      The positioning operation.
    /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
    move_result(cursor &cursor, move_operation operation, bool throw_notfound)
        : move_result(cursor, operation, slice::invalid(), slice::invalid(), throw_notfound) {}
    /// \brief Performs the given positioning operation at the specified key.
    /// \param cursor         The cursor to be moved.
    /// \param operation      The positioning operation.
    /// \param key            The key to position at.
    /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
    move_result(cursor &cursor, move_operation operation, const slice &key, bool throw_notfound)
        : move_result(cursor, operation, key, slice::invalid(), throw_notfound) {}
    /// \brief Performs the given positioning operation at the specified key-value pair.
    /// \param cursor         The cursor to be moved.
    /// \param operation      The positioning operation.
    /// \param key            The key to position at.
    /// \param value          The value to position at (for dupsort tables).
    /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
    inline move_result(cursor &cursor, move_operation operation, const slice &key, const slice &value,
                       bool throw_notfound);
    /// \brief Copy constructor.
    move_result(const move_result &) noexcept = default;
    /// \brief Copy assignment operator.
    /// \returns A reference to this instance.
    move_result &operator=(const move_result &) noexcept = default;
  };

  /// \brief The result of a range estimation: the boundary key-value pair plus
  /// the approximate number of items.
  struct estimate_result : public pair {
    ptrdiff_t approximate_quantity; ///< \brief The approximate number of items in the range.
    /// \brief Estimates the number of items from the current position per the given operation.
    /// \param cursor     The positioned cursor.
    /// \param operation  The positioning operation.
    estimate_result(const cursor &cursor, move_operation operation)
        : estimate_result(cursor, operation, slice::invalid(), slice::invalid()) {}
    /// \brief Estimates the number of items from the given key per the given operation.
    /// \param cursor     The positioned cursor.
    /// \param operation  The positioning operation.
    /// \param key        The boundary key.
    estimate_result(const cursor &cursor, move_operation operation, const slice &key)
        : estimate_result(cursor, operation, key, slice::invalid()) {}
    /// \brief Estimates the number of items from the given key-value pair per the given operation.
    /// \param cursor     The positioned cursor.
    /// \param operation  The positioning operation.
    /// \param key        The boundary key.
    /// \param value      The boundary value (for dupsort tables).
    inline estimate_result(const cursor &cursor, move_operation operation, const slice &key, const slice &value);
    /// \brief Copy constructor.
    estimate_result(const estimate_result &) noexcept = default;
    /// \brief Copy assignment operator.
    /// \returns A reference to this instance.
    estimate_result &operator=(const estimate_result &) noexcept = default;
  };

protected:
  /* fake `const`, but for specific move/get operations */
  /// \brief The low-level positioning primitive.
  /// \param operation      The positioning operation.
  /// \param key            The key argument (in/out).
  /// \param value          The value argument (in/out).
  /// \param throw_notfound Throw \ref mdbx::not_found instead of returning `false`.
  /// \returns `true` if the cursor was positioned successfully.
  /// \throws The corresponding \ref mdbx::exception on failure.
  inline bool move(move_operation operation, MDBX_val *key, MDBX_val *value, bool throw_notfound) const;

  /// \brief The low-level range estimation primitive.
  /// \param operation  The positioning operation.
  /// \param key        The boundary key (in/out).
  /// \param value      The boundary value (in/out).
  /// \returns The approximate number of items in the range.
  /// \throws The corresponding \ref mdbx::exception on failure.
  inline ptrdiff_t estimate(move_operation operation, MDBX_val *key, MDBX_val *value) const;

public:
  /// \brief Scans items calling the predicate until it returns `true`.
  /// \param predicate  The callable invoked for each key-value pair; returning
  ///                     `true` stops the scan.
  /// \param start      The initial positioning operation.
  /// \param turn       The operation for each next step.
  /// \returns `true` if the predicate returned `true` for some item.
  /// \throws The captured predicate exception, if any.
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

  /// \brief Scans the whole table calling the predicate until it returns `true`.
  /// \param predicate  The callable invoked for each key-value pair; returning
  ///                     `true` stops the scan.
  /// \param backward   Scan in the reverse order.
  /// \returns `true` if the predicate returned `true` for some item.
  /// \throws The captured predicate exception, if any.
  template <typename CALLABLE_PREDICATE> bool fullscan(CALLABLE_PREDICATE predicate, bool backward = false) {
    return scan_until(std::move(predicate), backward ? last : first, backward ? previous : next);
  }

  /// \brief Scans items starting from the given key, calling the predicate until
  /// it returns `true`.
  /// \param predicate  The callable invoked for each key-value pair; returning
  ///                     `true` stops the scan.
  /// \param from       The starting key; updated to the position where the scan stopped.
  /// \param start      The initial positioning operation.
  /// \param turn       The operation for each next step.
  /// \returns `true` if the predicate returned `true` for some item.
  /// \throws The captured predicate exception, if any.
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

  /// \brief Scans items starting from the given key-value pair, calling the
  /// predicate until it returns `true`.
  /// \param predicate  The callable invoked for each key-value pair; returning
  ///                     `true` stops the scan.
  /// \param from       The starting pair; updated to the position where the scan stopped.
  /// \param start      The initial positioning operation.
  /// \param turn       The operation for each next step.
  /// \returns `true` if the predicate returned `true` for some item.
  /// \throws The captured predicate exception, if any.
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

  /// \brief Performs the given positioning operation.
  /// \param operation      The positioning operation.
  /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result move(move_operation operation, bool throw_notfound) {
    return move_result(*this, operation, throw_notfound);
  }
  /// \brief Performs the given positioning operation at the specified key.
  /// \param operation      The positioning operation.
  /// \param key            The key to position at.
  /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result move(move_operation operation, const slice &key, bool throw_notfound) {
    return move_result(*this, operation, key, slice::invalid(), throw_notfound);
  }
  /// \brief Performs the given positioning operation at the specified key-value pair.
  /// \param operation      The positioning operation.
  /// \param key            The key to position at.
  /// \param value          The value to position at (for dupsort tables).
  /// \param throw_notfound Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result move(move_operation operation, const slice &key, const slice &value, bool throw_notfound) {
    return move_result(*this, operation, key, value, throw_notfound);
  }
  /// \brief Deleted: `throw_notfound` must be specified explicitly to avoid
  /// accidental boolean conversions.
  move_result move(move_operation operation, const slice &key, const slice &value) = delete;
  /// \brief Performs the given positioning operation updating the key and value slices.
  /// \param operation      The positioning operation.
  /// \param key            The key slice to be updated with the found key.
  /// \param value          The value slice to be updated with the found value.
  /// \param throw_notfound Throw \ref mdbx::not_found instead of returning `false`.
  /// \returns `true` if the cursor was positioned successfully.
  template <class SLICE, typename ::std::enable_if<::std::is_same<SLICE, slice>::value, int>::type = 0>
  bool move(move_operation operation, SLICE &key, SLICE &value, bool throw_notfound) {
    return move(operation, &key, &value, throw_notfound);
  }

  /// \brief Position the cursor at the first key/data item.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_first(bool throw_notfound = true) { return move(first, throw_notfound); }
  /// \brief Position the cursor at the previous key/data item.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_previous(bool throw_notfound = true) { return move(previous, throw_notfound); }
  /// \brief Position the cursor at the last multi-value of the previous key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_previous_last_multi(bool throw_notfound = true) {
    return move(multi_prevkey_lastvalue, throw_notfound);
  }
  /// \brief Position the cursor at the first multi-value of the current key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_current_first_multi(bool throw_notfound = true) {
    return move(multi_currentkey_firstvalue, throw_notfound);
  }
  /// \brief Position the cursor at the previous multi-value of the current key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_current_prev_multi(bool throw_notfound = true) {
    return move(multi_currentkey_prevvalue, throw_notfound);
  }
  /// \brief Read the current key/data item without moving.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result current(bool throw_notfound = true) const { return move_result(*this, throw_notfound); }
  /// \brief Position the cursor at the next multi-value of the current key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_current_next_multi(bool throw_notfound = true) {
    return move(multi_currentkey_nextvalue, throw_notfound);
  }
  /// \brief Position the cursor at the last multi-value of the current key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_current_last_multi(bool throw_notfound = true) {
    return move(multi_currentkey_lastvalue, throw_notfound);
  }
  /// \brief Position the cursor at the first multi-value of the next key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_next_first_multi(bool throw_notfound = true) { return move(multi_nextkey_firstvalue, throw_notfound); }
  /// \brief Position the cursor at the next key/data item.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_next(bool throw_notfound = true) { return move(next, throw_notfound); }
  /// \brief Position the cursor at the last key/data item.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_last(bool throw_notfound = true) { return move(last, throw_notfound); }

  /// \brief Position the cursor at the key lesser than the given one.
  /// \param key             The key to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_key_lesser_than(const slice &key, bool throw_notfound = true) {
    return move(key_lesser_than, key, throw_notfound);
  }
  /// \brief Position the cursor at the key lesser than or equal to the given one.
  /// \param key             The key to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_key_lesser_or_equal(const slice &key, bool throw_notfound = true) {
    return move(key_lesser_or_equal, key, throw_notfound);
  }
  /// \brief Position the cursor at the key equal to the given one.
  /// \param key             The key to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_key_equal(const slice &key, bool throw_notfound = true) {
    return move(key_equal, key, throw_notfound);
  }
  /// \brief Position the cursor at the key exactly matching the given one.
  /// \param key             The key to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_key_exact(const slice &key, bool throw_notfound = true) {
    return move(key_exact, key, throw_notfound);
  }
  /// \brief Position the cursor at the key greater than or equal to the given one.
  /// \param key             The key to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_key_greater_or_equal(const slice &key, bool throw_notfound = true) {
    return move(key_greater_or_equal, key, throw_notfound);
  }
  /// \brief Position the cursor at the key greater than the given one.
  /// \param key             The key to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_key_greater_than(const slice &key, bool throw_notfound = true) {
    return move(key_greater_than, key, throw_notfound);
  }

  /// \brief Position the cursor at the exact key and the multi-value lesser than the given one.
  /// \param key             The exact key to position at.
  /// \param value           The multi-value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_exact_key_value_lesser_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_lesser_than, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the exact key and the multi-value lesser than or equal to the given one.
  /// \param key             The exact key to position at.
  /// \param value           The multi-value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_exact_key_value_lesser_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_lesser_or_equal, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the exact key and the multi-value equal to the given one.
  /// \param key             The exact key to position at.
  /// \param value           The multi-value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_exact_key_value_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_equal, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the exact key and the multi-value greater than or equal to the given one.
  /// \param key             The exact key to position at.
  /// \param value           The multi-value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_exact_key_value_greater_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_greater_or_equal, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the exact key and the multi-value greater than the given one.
  /// \param key             The exact key to position at.
  /// \param value           The multi-value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_exact_key_value_greater_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(multi_exactkey_value_greater, key, value, throw_notfound);
  }

  /// \brief Position the cursor at the key-value pair lesser than the given one.
  /// \param key             The key to position against.
  /// \param value           The value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_pair_lesser_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_lesser_than, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the key-value pair lesser than or equal to the given one.
  /// \param key             The key to position against.
  /// \param value           The value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_pair_lesser_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_lesser_or_equal, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the key-value pair equal to the given one.
  /// \param key             The key to position against.
  /// \param value           The value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_pair_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_equal, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the key-value pair exactly matching the given one.
  /// \param key             The key to position against.
  /// \param value           The value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_pair_exact(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_exact, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the key-value pair greater than or equal to the given one.
  /// \param key             The key to position against.
  /// \param value           The value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_pair_greater_or_equal(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_greater_or_equal, key, value, throw_notfound);
  }
  /// \brief Position the cursor at the key-value pair greater than the given one.
  /// \param key             The key to position against.
  /// \param value           The value to position against.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  move_result to_pair_greater_than(const slice &key, const slice &value, bool throw_notfound = true) {
    return move(pair_greater_than, key, value, throw_notfound);
  }

  /// \brief Seeks the cursor to the exact key.
  /// \param key  The key to seek.
  /// \returns `true` if the exact key was found.
  inline bool seek(const slice &key);
  /// \brief Finds the exact key.
  /// \param key             The key to find.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  inline move_result find(const slice &key, bool throw_notfound = true);
  /// \brief Finds the exact key or the first one greater than it.
  /// \param key             The key to find.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  inline move_result lower_bound(const slice &key, bool throw_notfound = false);
  /// \brief Finds the first key greater than the given one.
  /// \param key             The key to find the upper bound for.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  inline move_result upper_bound(const slice &key, bool throw_notfound = false);

  /// \brief Return count of duplicates for current key.
  /// \returns The number of multi-values at the current key.
  inline size_t count_multivalue() const;
  /// \brief Return count of duplicates for current key, along with basic
  /// statistics of the table. \see ::mdbx_cursor_count_ex()
  /// \param stat  Optional address of an \ref MDBX_stat structure to be filled.
  /// \returns The number of multi-values at the current key.
  inline size_t count_multivalue(MDBX_stat *stat) const;

  /// \brief Reads a batch of key-value pairs.
  ///
  /// \details Fetches up to `max_pairs` key-value pairs starting from the
  /// current cursor position, advancing the cursor. The returned slices
  /// reference database-owned memory and remain valid while the transaction
  /// and cursor are alive.
  ///
  /// \param [in] max_pairs  The maximum number of key-value pairs to fetch.
  /// \param [in] op         The positioning operation, either
  ///                        `move_operation::first` or `move_operation::next`.
  /// \param [out] is_last   Optional flag set to `true` when the returned chunk
  ///                        is the last one (no pairs left after it).
  ///
  /// \returns The vector of fetched key-value pairs.
  /// \see ::mdbx_cursor_get_batch()
  inline ::std::vector<pair> get_batch(size_t max_pairs, move_operation op, bool *is_last = nullptr) const;

  /// \brief Disables the control of the order of keys when reading database
  /// pages for this cursor.
  ///
  /// \details Useful when reading a database whose user-defined comparison
  /// functions are unavailable, otherwise such reads return \ref MDBX_CORRUPTED.
  /// \see ::mdbx_cursor_ignord()
  inline void ignore_key_order();

  /// \brief Finds the exact key-value pair in a dupsort table.
  /// \param key             The key to find.
  /// \param value           The multi-value to find.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  inline move_result find_multivalue(const slice &key, const slice &value, bool throw_notfound = true);
  /// \brief Finds the exact key and the first multi-value greater-or-equal to the given one.
  /// \param key             The key to find.
  /// \param value           The multi-value lower bound.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  inline move_result lower_bound_multivalue(const slice &key, const slice &value, bool throw_notfound = false);
  /// \brief Finds the exact key and the first multi-value greater than the given one.
  /// \param key             The key to find.
  /// \param value           The multi-value to find the upper bound for.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the found pair and the success flag.
  inline move_result upper_bound_multivalue(const slice &key, const slice &value, bool throw_notfound = false);

  /// \brief Seeks and returns a batch of same-length multi-values of the given key.
  /// \param key             The key of the multi-value hive.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the batch and the success flag.
  inline move_result seek_multiple_samelength(const slice &key, bool throw_notfound = true) {
    return move(seek_and_batch_samelength, key, throw_notfound);
  }

  /// \brief Returns a batch of same-length multi-values at the current key.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the batch and the success flag.
  inline move_result get_multiple_samelength(bool throw_notfound = false) {
    return move(batch_samelength, throw_notfound);
  }

  /// \brief Moves to the next key and returns its batch of same-length multi-values.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the batch and the success flag.
  inline move_result next_multiple_samelength(bool throw_notfound = false) {
    return move(batch_samelength_next, throw_notfound);
  }

  /// \brief Moves to the previous key and returns its batch of same-length multi-values.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning an unsuccessful result.
  /// \returns The \ref move_result with the batch and the success flag.
  inline move_result previous_multiple_samelength(bool throw_notfound = false) {
    return move(batch_samelength_previous, throw_notfound);
  }

  /// \brief Checks whether the cursor is at the end-of-table (not positioned).
  /// \returns `true` if the cursor is not positioned on any item.
  inline bool eof() const;
  /// \brief Checks whether the cursor is on the first item of the table.
  /// \returns `true` if the cursor is on the first item.
  inline bool on_first() const;
  /// \brief Checks whether the cursor is on the last item of the table.
  /// \returns `true` if the cursor is on the last item.
  inline bool on_last() const;
  /// \brief Checks whether the cursor is on the first multi-value of the current key.
  /// \returns `true` if the cursor is on the first multi-value.
  inline bool on_first_multival() const;
  /// \brief Checks whether the cursor is on the last multi-value of the current key.
  /// \returns `true` if the cursor is on the last multi-value.
  inline bool on_last_multival() const;
  /// \brief Estimates the number of items between the current position and the given key-value pair.
  /// \param key    The boundary key.
  /// \param value  The boundary value.
  /// \returns The \ref estimate_result with the approximate quantity.
  inline estimate_result estimate(const slice &key, const slice &value) const;
  /// \brief Estimates the number of items between the current position and the given key.
  /// \param key  The boundary key.
  /// \returns The \ref estimate_result with the approximate quantity.
  inline estimate_result estimate(const slice &key) const;
  /// \brief Estimates the number of items up to the boundary defined by the operation.
  /// \param operation  The positioning operation defining the boundary.
  /// \returns The \ref estimate_result with the approximate quantity.
  inline estimate_result estimate(move_operation operation) const;
  /// \brief Estimates the number of items up to the boundary defined by the operation and key.
  /// \param operation  The positioning operation defining the boundary.
  /// \param key        The boundary key.
  /// \returns The \ref estimate_result with the approximate quantity.
  inline estimate_result estimate(move_operation operation, const slice &key) const;

  /// \brief Estimates the distance (number of items) between two cursor positions.
  /// \param from      The cursor at the "from" position, null means the table start.
  /// \param to        The cursor at the "to" position, null means the table end.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns The approximate distance; a negative value means `to` is before `from`.
  static inline ptrdiff_t distance_between(const cursor from, const cursor to,
                                           unsigned deepness = /* enough to cover whole tree height */ 42);
  /// \brief Estimates the distance from the given cursor position to this one.
  /// \param from      The cursor at the "from" position, null means the table start.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns The approximate distance.
  inline ptrdiff_t distance_from(const cursor from,
                                 unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(from, *this, deepness);
  }
  /// \brief Estimates the distance from this cursor position to the given one.
  /// \param to        The cursor at the "to" position, null means the table end.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns The approximate distance.
  inline ptrdiff_t distance_to(const cursor to, unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(*this, to, deepness);
  }
  /// \brief Estimates the distance from the first item of the table to this cursor position.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns The approximate distance.
  inline ptrdiff_t distance_from_first(unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(nullptr, *this, deepness);
  }
  /// \brief Estimates the distance from this cursor position to the end of the table.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns The approximate distance.
  inline ptrdiff_t distance_to_end(unsigned deepness = /* enough to cover whole tree height */ 42) const {
    return distance_between(*this, nullptr, deepness);
  }

  /// \brief Scrolls the cursor to the given distance from the current position.
  /// \param distable        The signed number of items to scroll by.
  /// \param deepness        The maximum depth of the B+tree inspection.
  /// \param throw_notfound  Throw \ref mdbx::not_found instead of returning `false`.
  /// \returns `true` if the cursor was positioned successfully.
  inline bool scroll(intptr_t distable, unsigned deepness = /* enough to cover whole tree height */ 42,
                     bool throw_notfound = true);

  /// \brief Distributes cursors evenly over the range between two positions.
  /// \param from               The cursor at the start of the range, null means the table start.
  /// \param to                 The cursor at the end of the range, null means the table end.
  /// \param cursors_array      The array of cursors to be positioned.
  /// \param cursors_array_size The number of cursors in the array.
  /// \param deepness           The maximum depth of the B+tree inspection.
  /// \returns `true` on success.
  static inline bool distribute(const cursor from, const cursor to, cursor *cursors_array, intptr_t cursors_array_size,
                                unsigned deepness = /* enough to cover whole tree height */ 42);

  /// \brief Distributes cursors evenly over the range between two positions.
  /// \param from      The cursor at the start of the range, null means the table start.
  /// \param to        The cursor at the end of the range, null means the table end.
  /// \param cursors   The vector of cursors to be positioned.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns `true` on success.
  static inline bool distribute(const cursor from, const cursor to, const std::vector<cursor> &cursors,
                                unsigned deepness = /* enough to cover whole tree height */ 42);

  /// \brief Distributes cursors evenly over the range between two positions.
  /// \param from      The cursor at the start of the range, null means the table start.
  /// \param to        The cursor at the end of the range, null means the table end.
  /// \param cursors   The vector of managed cursors to be positioned.
  /// \param deepness  The maximum depth of the B+tree inspection.
  /// \returns `true` on success.
  static inline bool distribute(const cursor from, const cursor to, const std::vector<cursor_managed> &cursors,
                                unsigned deepness = /* enough to cover whole tree height */ 42);

  //----------------------------------------------------------------------------

  /// \brief Renew/bind a cursor with a new transaction and previously used key-value map handle.
  /// \param txn  The new transaction to attach the cursor to.
  /// \throws mdbx::error on failure. After \ref unbind() or after
  ///   `txn::unbind_all_cursors()` this is how the cursor becomes usable again.
  inline void renew(::mdbx::txn &txn);

  /// \brief Bind/renew a cursor with a new transaction and specified key-value map handle.
  /// \param txn          The new transaction to attach the cursor to.
  /// \param map_handle   The key-value map (table) handle.
  /// \throws mdbx::error on failure (issue #146).
  inline void bind(::mdbx::txn &txn, ::mdbx::map_handle map_handle);

  /// \brief Unbind cursor from a transaction.
  /// \details Detaches the cursor from its transaction without closing the
  ///   underlying C object: the wrapper keeps the handle and the destructor
  ///   still closes it. While unbound, any seek/move/read operation fails with
  ///   \ref MDBX_EBADSIGN or \ref MDBX_EINVAL; call \ref renew() / \ref bind()
  ///   to attach the cursor to a transaction again (issue #146).
  inline void unbind();

  /// \brief Returns the cursor's transaction.
  /// \returns The transaction the cursor belongs to.
  inline ::mdbx::txn txn() const;
  /// \brief Returns the cursor's table (map) handle.
  /// \returns The \ref map_handle the cursor is associated with.
  inline map_handle map() const;

  /// \brief Converts the cursor to its transaction.
  /// \returns The transaction the cursor belongs to.
  inline operator ::mdbx::txn() const { return txn(); }
  /// \brief Converts the cursor to its table handle.
  /// \returns The \ref map_handle the cursor is associated with.
  inline operator ::mdbx::map_handle() const { return map(); }

  /// \brief Stores a key-value pair using the given low-level flags.
  /// \param key    The key to store.
  /// \param value  The value to store (in/out for some flags).
  /// \param flags  The \ref MDBX_put_flags_t combination.
  /// \returns An error code, zero on success (does not throw).
  inline MDBX_error_t put(const slice &key, slice *value, MDBX_put_flags_t flags) noexcept;
  /// \brief Stores a key-value pair in the given mode.
  /// \param key    The key to store.
  /// \param value  The value to store.
  /// \param mode   The \ref put_mode (insert/upsert/update).
  /// \throws mdbx::error on failure.
  inline void put(const slice &key, slice value, put_mode mode);
  /// \brief Inserts a new key-value pair; fails if the key already exists.
  /// \param key    The key to insert.
  /// \param value  The value to insert.
  /// \throws mdbx::key_exists if the key already exists.
  inline void insert(const slice &key, slice value);
  /// \brief Tries to insert a new key-value pair without throwing on a duplicate.
  /// \param key    The key to insert.
  /// \param value  The value to insert.
  /// \returns The \ref value_result with the success flag.
  inline value_result try_insert(const slice &key, slice value);
  /// \brief Reserves space for a value of the given length for a new key.
  /// \param key           The key to insert.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  /// \throws mdbx::key_exists if the key already exists.
  inline slice insert_reserve(const slice &key, size_t value_length);
  /// \brief Tries to reserve space for a value of the given length for a new key.
  /// \param key           The key to insert.
  /// \param value_length  The length of the value to reserve.
  /// \returns The \ref value_result with the reserved slice and the success flag.
  inline value_result try_insert_reserve(const slice &key, size_t value_length);

  /// \brief Inserts or updates the value for the given key.
  /// \param key    The key to store.
  /// \param value  The value to store.
  /// \throws mdbx::error on failure.
  inline void upsert(const slice &key, const slice &value);
  /// \brief Reserves space for a value of the given length, inserting or updating the key.
  /// \param key           The key to store.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  inline slice upsert_reserve(const slice &key, size_t value_length);

  /// \brief Updates value associated with a key at the current cursor position.
  /// \param value  The new value.
  void update_current(const slice &value);
  /// \brief Reserves and returns the space for storing a value associated with a key at the current cursor position.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  slice reserve_current(size_t value_length);
  /// \brief Deprecated misspelling of \ref reserve_current().
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  slice reverse_current(size_t value_length) { return reserve_current(value_length); }

  /// \brief Updates the value for an existing key; fails if the key is absent.
  /// \param key    The key to update.
  /// \param value  The new value.
  /// \throws mdbx::not_found if the key does not exist.
  inline void update(const slice &key, const slice &value);
  /// \brief Tries to update the value for an existing key without throwing.
  /// \param key    The key to update.
  /// \param value  The new value.
  /// \returns `true` if the key was found and updated.
  inline bool try_update(const slice &key, const slice &value);
  /// \brief Reserves space for a value of the given length for an existing key.
  /// \param key           The key to update.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  /// \throws mdbx::not_found if the key does not exist.
  inline slice update_reserve(const slice &key, size_t value_length);
  /// \brief Tries to reserve space for a value of the given length for an existing key.
  /// \param key           The key to update.
  /// \param value_length  The length of the value to reserve.
  /// \returns The \ref value_result with the reserved slice and the success flag.
  inline value_result try_update_reserve(const slice &key, size_t value_length);

  /// \brief Stores a key-value pair in the given mode.
  /// \param kv    The key-value pair.
  /// \param mode  The \ref put_mode (insert/upsert/update).
  void put(const pair &kv, put_mode mode) { return put(kv.key, kv.value, mode); }
  /// \brief Inserts a new key-value pair; fails if the key already exists.
  /// \param kv  The key-value pair to insert.
  void insert(const pair &kv) { return insert(kv.key, kv.value); }
  /// \brief Tries to insert a new key-value pair without throwing on a duplicate.
  /// \param kv  The key-value pair to insert.
  /// \returns The \ref value_result with the success flag.
  value_result try_insert(const pair &kv) { return try_insert(kv.key, kv.value); }
  /// \brief Inserts or updates the value for the given key.
  /// \param kv  The key-value pair to store.
  void upsert(const pair &kv) { return upsert(kv.key, kv.value); }

  /// \brief Removes single key-value pair or all multi-values at the current cursor position.
  /// \param whole_multivalue  Remove all multi-values of the current key.
  /// \returns `true` if something was removed.
  inline bool erase(bool whole_multivalue = false);

  /// \brief Seeks and removes first value or whole multi-value of the given key.
  /// \param key               The key to remove.
  /// \param whole_multivalue  Remove all multi-values of the key.
  /// \return `True` if the key is found and a value(s) is removed.
  inline bool erase(const slice &key, bool whole_multivalue = true);

  /// \brief Seeks and removes the particular multi-value entry of the key.
  /// \param key    The key of the entry.
  /// \param value  The multi-value to remove.
  /// \return `True` if the given key-value pair is found and removed.
  inline bool erase(const slice &key, const slice &value);

  /// \brief Overloads for C strings, which otherwise bind to the `bool` parameters above.
  /// \param key               The key to remove.
  /// \param whole_multivalue  Remove all multi-values of the key.
  /// \returns `true` if the key was found and removed.
  template <typename CHARS, typename BOOL = bool,
            typename std::enable_if<(std::is_same<CHARS, const char *>::value || std::is_same<CHARS, char *>::value) &&
                                        std::is_same<BOOL, bool>::value,
                                    int>::type = 0>
  bool erase(CHARS key, BOOL whole_multivalue = true) {
    return erase(slice(key), whole_multivalue);
  }
  /// \brief Removes the particular multi-value entry given by C string literals.
  /// \param key    The key of the entry.
  /// \param value  The multi-value to remove.
  /// \returns `true` if the key-value pair was found and removed.
  template <size_t SIZE> bool erase(const slice &key, const char (&value)[SIZE]) { return erase(key, slice(value)); }
  /// \brief Removes the particular multi-value entry given by C string literals.
  /// \param key    The key of the entry.
  /// \param value  The multi-value to remove.
  /// \returns `true` if the key-value pair was found and removed.
  template <size_t KEY_SIZE, size_t VALUE_SIZE>
  bool erase(const char (&key)[KEY_SIZE], const char (&value)[VALUE_SIZE]) {
    return erase(slice(key), slice(value));
  }

  /// \brief Modes for deleting bunches of neighboring items, see
  /// \ref ::MDBX_bunch_action_t(). The EXCLUDING and INCLUDING suffixes mean
  /// correspondingly excluding and including deletion items in the current
  /// cursor position.
  enum bunch_delete {
    delete_current_value = MDBX_DELETE_CURRENT_VALUE,
    delete_current_multival_before_excluding = MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_EXCLUDING,
    delete_current_multival_before_including = MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_INCLUDING,
    delete_current_multival_after_including = MDBX_DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING,
    delete_current_multival_after_excluding = MDBX_DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING,
    delete_current_multival_all = MDBX_DELETE_CURRENT_MULTIVAL_ALL,
    delete_before_excluding = MDBX_DELETE_BEFORE_EXCLUDING,
    delete_before_including = MDBX_DELETE_BEFORE_INCLUDING,
    delete_after_including = MDBX_DELETE_AFTER_INCLUDING,
    delete_after_excluding = MDBX_DELETE_AFTER_EXCLUDING,
    delete_whole = MDBX_DELETE_WHOLE,
  };

  /// \brief Quickly removes bunches of neighboring items much faster by
  /// cutting out entire pages and branches from the B+ tree structure.
  ///
  /// \param action  The \ref bunch_delete mode defining what to remove.
  /// \returns The number of removed items.
  /// \see erase_range() \see ::mdbx_cursor_bunch_delete()
  inline size_t erase_bunch(bunch_delete action);

  /// \brief Quickly removes a range of items between the current cursor
  /// position and the position of `end` much faster by cutting out entire
  /// pages and branches from the B+ tree structure.
  ///
  /// \param [in] end  A positioned cursor defining the end of the range.
  /// \param [in] end_including  Whether the `end` position itself should be
  /// included in the range to be deleted.
  /// \returns The number of removed items.
  /// \see erase_bunch() \see ::mdbx_cursor_delete_range()
  inline size_t erase_range(const cursor &end, bool end_including);

  /// \brief Resets the cursor state.
  ///
  /// \details As a result of the reset, the cursor becomes unpositioned and
  /// does not allow relative positioning operations, getting or changing data
  /// until the cursor is set to a position independent of the current one.
  /// \see ::mdbx_cursor_reset()
  inline void reset();

  /// \brief Stores a bunch of same-length multi-values for the given key.
  /// \param key             The key of the multi-value hive.
  /// \param value_length    The length of each value in bytes.
  /// \param values_array    The array of values.
  /// \param values_count    The number of values in the array.
  /// \param mode            The \ref put_mode.
  /// \param allow_partial   Allow partial store when some values are rejected.
  /// \returns The number of stored values.
  inline size_t put_multiple_samelength(const slice &key, const size_t value_length, const void *values_array,
                                        size_t values_count, put_mode mode, bool allow_partial = false);
  /// \brief Stores a bunch of same-length multi-values for the given key.
  /// \param key             The key of the multi-value hive.
  /// \param values_array    The array of standard-layout values.
  /// \param values_count    The number of values in the array.
  /// \param mode            The \ref put_mode.
  /// \param allow_partial   Allow partial store when some values are rejected.
  /// \returns The number of stored values.
  template <typename VALUE>
  size_t put_multiple_samelength(const slice &key, const VALUE *values_array, size_t values_count, put_mode mode,
                                 bool allow_partial = false) {
    static_assert(::std::is_standard_layout<VALUE>::value && !::std::is_pointer<VALUE>::value &&
                      !::std::is_array<VALUE>::value,
                  "Must be a standard layout type!");
    return put_multiple_samelength(key, sizeof(VALUE), values_array, values_count, mode, allow_partial);
  }
  /// \brief Stores a bunch of same-length multi-values for the given key.
  /// \param key     The key of the multi-value hive.
  /// \param vector  The vector of standard-layout values.
  /// \param mode    The \ref put_mode.
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
  /// \brief Delegated constructor for RAII, wraps the given C handle.
  /// \param ptr  The pointer to the underlying \ref MDBX_cursor object.
  MDBX_CXX11_CONSTEXPR cursor_managed(MDBX_cursor *ptr) noexcept : inherited(ptr) {}

public:
  /// \brief Creates a new managed cursor with underlying object.
  /// \param your_context  An application context to be associated with the cursor.
  /// \throws mdbx::error on failure to allocate the underlying object.
  cursor_managed(void *your_context = nullptr) : cursor_managed(::mdbx_cursor_create(your_context)) {
    if (MDBX_UNLIKELY(!handle_))
      MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_ENOMEM);
  }

  /// \brief Explicitly closes the cursor.
  void close();

  /// \brief Move constructor.
  cursor_managed(cursor_managed &&) = default;
  /// \brief Move assignment operator.
  /// \param other  The source managed cursor to be moved from.
  /// \returns A reference to this instance.
  cursor_managed &operator=(cursor_managed &&other) noexcept;

  /// \brief Withdraws (takes over) the underlying C handle; this instance
  /// becomes empty and will not close the cursor.
  /// \returns The pointer to the underlying \ref MDBX_cursor object.
  inline MDBX_cursor *withdraw_handle() noexcept {
    MDBX_cursor *handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  /// \brief Copy construction is prohibited.
  cursor_managed(const cursor_managed &) = delete;
  /// \brief Copy assignment is prohibited.
  cursor_managed &operator=(const cursor_managed &) = delete;
  /// \brief Destroys the managed cursor, closing the underlying object.
  ~cursor_managed();
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
