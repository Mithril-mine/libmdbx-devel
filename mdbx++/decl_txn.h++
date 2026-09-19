// > dist-cutoff-begin
#pragma once
#include "decl_buffer.h++"
#include "decl_core.h++"
#include "decl_env.h++"
#include "decl_exceptions.h++"
#include "decl_slice.h++"
#include "decl_transcoders.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief Unmanaged database transaction.
///
/// Like other unmanaged classes, `txn` allows copying and assignment for handles as a values,
/// but does not manage nor destroys the represented underlying object from the own class destructor.
///
/// All database operations require a transaction handle. Transactions may be
/// read-only or read-write.
class LIBMDBX_API_TYPE txn {
protected:
  friend class cursor;
  MDBX_txn *handle_{nullptr};
  MDBX_CXX11_CONSTEXPR txn(MDBX_txn *ptr) noexcept;

public:
  MDBX_CXX11_CONSTEXPR txn() noexcept = default;
  txn(const txn &) noexcept = default;
  txn &operator=(const txn &) noexcept = default;
  inline txn &operator=(txn &&) noexcept;
  inline txn(txn &&) noexcept;
  inline ~txn() noexcept;

  MDBX_CXX14_CONSTEXPR operator bool() const noexcept { return handle_ != nullptr; };
  MDBX_CXX14_CONSTEXPR operator const MDBX_txn *() const noexcept { return handle_; }
  MDBX_CXX14_CONSTEXPR operator MDBX_txn *() noexcept { return handle_; }
  MDBX_CXX14_CONSTEXPR const MDBX_txn *handle() const noexcept { return handle_; }
  MDBX_CXX14_CONSTEXPR MDBX_txn *handle() noexcept { return handle_; };
  friend MDBX_CXX11_CONSTEXPR bool operator==(const txn &a, const txn &b) noexcept { return a.handle_ == b.handle_; }
  friend MDBX_CXX11_CONSTEXPR bool operator!=(const txn &a, const txn &b) noexcept { return a.handle_ != b.handle_; }

  /// \brief Returns the transaction's environment.
  inline ::mdbx::env env() const noexcept;
  /// \brief Returns transaction's flags.
  inline MDBX_txn_flags_t flags() const;
  /// \brief Return the transaction's ID.
  inline uint64_t id() const;

  /// \brief Returns the application context associated with the transaction.
  inline void *get_context() const noexcept;

  /// \brief Sets the application context associated with the transaction.
  inline txn &set_context(void *your_context);

  /// \brief Checks whether the given data is on a dirty page.
  inline bool is_dirty(const void *ptr) const;

  /// \brief Checks whether the given slice is on a dirty page.
  inline bool is_dirty(const slice &item) const { return item && is_dirty(item.data()); }

  /// \brief Checks whether the transaction is read-only.
  bool is_readonly() const { return (flags() & MDBX_TXN_RDONLY) != 0; }

  /// \brief Checks whether the transaction is read-write.
  bool is_readwrite() const { return (flags() & MDBX_TXN_RDONLY) == 0; }

  using info = ::MDBX_txn_info;
  /// \brief Returns information about the MDBX transaction.
  inline info get_info(bool scan_reader_lock_table = false) const;

  /// \brief Returns maximal write transaction size (i.e. limit for summary
  /// volume of dirty pages) in bytes.
  size_t size_max() const { return env().transaction_size_max(); }

  /// \brief Returns current write transaction size (i.e.summary volume of dirty pages) in bytes.
  size_t size_current() const {
    assert(is_readwrite());
    return size_t(get_info().txn_space_dirty);
  }

  //----------------------------------------------------------------------------

  /// \brief Reset read-only transaction.
  inline void reset_reading();

  /// \brief Renew read-only transaction.
  inline void renew_reading();

  /// \brief Clone read transaction.
  inline txn_managed clone(void *context = nullptr) const;

  /// \brief Renew given read transaction into clone.
  inline void clone(txn_managed &txn_for_renew_into_clone, void *context = nullptr) const;

  /// \brief Copy an MVCC-snapshot of the database to the destination path.
  ///
  /// \details Copies the consistent state as seen by this transaction,
  /// see \ref ::mdbx_txn_copy2pathname().
  /// \see env::copy()
  inline void copy(const char *destination, bool compactify, bool force_dynamic_size = false);
  /// \copydoc copy(const char *, bool, bool)
  inline void copy(const ::std::string &destination, bool compactify, bool force_dynamic_size = false);
#ifdef MDBX_STD_FILESYSTEM_PATH
  /// \copydoc copy(const char *, bool, bool)
  inline void copy(const MDBX_STD_FILESYSTEM_PATH &destination, bool compactify, bool force_dynamic_size = false);
#endif /* MDBX_STD_FILESYSTEM_PATH */
#if defined(_WIN32) || defined(_WIN64) || defined(DOXYGEN)
  /// \copydoc copy(const char *, bool, bool)
  inline void copy(const wchar_t *destination, bool compactify, bool force_dynamic_size = false);
  /// \copydoc copy(const char *, bool, bool)
  inline void copy(const ::std::wstring &destination, bool compactify, bool force_dynamic_size = false);
#endif /* Windows */

  /// \brief Copy an MVCC-snapshot of the database to the given file handle.
  /// \see ::mdbx_txn_copy2fd()
  inline void copy(filehandle fd, bool compactify, bool force_dynamic_size = false);

  /// \brief Marks transaction as broken to prevent further operations.
  inline void make_broken();

  /// \brief Park read-only transaction.
  inline void park_reading(bool autounpark = true);

  /// \brief Resume parked read-only transaction.
  /// \returns True if transaction was restarted while `restart_if_ousted=true`.
  inline bool unpark_reading(bool restart_if_ousted = true);

  /// \brief Start nested write transaction.
  txn_managed start_nested();

  /// \brief Start nested transaction.
  txn_managed start_nested(bool readonly);

  /// \brief Opens cursor for specified key-value map handle.
  inline cursor_managed open_cursor(map_handle map) const;

  /// \brief Unbind or close all cursors.
  inline size_t release_all_cursors(bool unbind) const;

  /// \brief Close all cursors.
  inline size_t close_all_cursors() const { return release_all_cursors(false); }

  /// \brief Unbind all cursors.
  inline size_t unbind_all_cursors() const { return release_all_cursors(true); }

  /// \brief Open existing key-value map.
  inline map_handle open_map(const char *name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const;
  /// \brief Open existing key-value map.
  inline map_handle open_map(const ::std::string &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const;
  /// \brief Open existing key-value map.
  inline map_handle open_map(const slice &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const;

  /// \brief Open existing key-value map.
  inline map_handle open_map_accede(const char *name) const;
  /// \brief Open existing key-value map.
  inline map_handle open_map_accede(const ::std::string &name) const;
  /// \brief Open existing key-value map.
  inline map_handle open_map_accede(const slice &name) const;

  /// \brief Create new or open existing key-value map.
  inline map_handle create_map(const char *name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single);
  /// \brief Create new or open existing key-value map.
  inline map_handle create_map(const ::std::string &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single);
  /// \brief Create new or open existing key-value map.
  inline map_handle create_map(const slice &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single);

  /// \brief Drops key-value map using handle.
  inline void drop_map(map_handle map);
  /// \brief Drops key-value map using name.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  bool drop_map(const char *name, bool throw_if_absent = false);
  /// \brief Drop key-value map.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  inline bool drop_map(const ::std::string &name, bool throw_if_absent = false);
  /// \brief Drop key-value map.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  bool drop_map(const slice &name, bool throw_if_absent = false);

  /// \brief Clear key-value map.
  inline void clear_map(map_handle map);
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  bool clear_map(const char *name, bool throw_if_absent = false);
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  inline bool clear_map(const ::std::string &name, bool throw_if_absent = false);
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  bool clear_map(const slice &name, bool throw_if_absent = false);

  /// \brief Переименовывает таблицу ключ-значение.
  inline void rename_map(map_handle map, const char *new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  inline void rename_map(map_handle map, const ::std::string &new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  inline void rename_map(map_handle map, const slice &new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  bool rename_map(const char *old_name, const char *new_name, bool throw_if_absent = false);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  bool rename_map(const ::std::string &old_name, const ::std::string &new_name, bool throw_if_absent = false);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  bool rename_map(const slice &old_name, const slice &new_name, bool throw_if_absent = false);

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)

  /// \brief Open existing key-value map.
  inline map_handle open_map(const ::std::string_view &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const {
    return open_map(slice(name), key_mode, value_mode);
  }
  /// \brief Open existing key-value map.
  inline map_handle open_map_accede(const ::std::string_view &name) const;
  /// \brief Create new or open existing key-value map.
  inline map_handle create_map(const ::std::string_view &name,
                               const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) {
    return create_map(slice(name), key_mode, value_mode);
  }
  /// \brief Drop key-value map.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  bool drop_map(const ::std::string_view &name, bool throw_if_absent = false) {
    return drop_map(slice(name), throw_if_absent);
  }
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  bool clear_map(const ::std::string_view &name, bool throw_if_absent = false) {
    return clear_map(slice(name), throw_if_absent);
  }
  /// \brief Переименовывает таблицу ключ-значение.
  inline void rename_map(map_handle map, const ::std::string_view &new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  bool rename_map(const ::std::string_view &old_name, const ::std::string_view &new_name,
                  bool throw_if_absent = false) {
    return rename_map(slice(old_name), slice(new_name), throw_if_absent);
  }
#endif /* __cpp_lib_string_view >= 201606L */

  using map_stat = ::MDBX_stat;
  /// \brief Returns statistics for a table.
  inline map_stat get_map_stat(map_handle map) const;
  /// \brief Returns depth (bitmask) information of nested dupsort (multi-value)
  /// B+trees for given table.
  inline uint32_t get_tree_deepmask(map_handle map) const;
  /// \brief Returns information about key-value map (aka table) handle.
  inline map_handle::info get_map_flags(map_handle map) const;

  /// \brief Enumerates user's named tables in a database.
  ///
  /// \details Calls the `visitor` functor for each user-created named table
  /// until the named tables are exhausted, or until the visitor returns
  /// \ref exit_loop (which will be returned as a result).
  ///
  /// \param [in,out] visitor  A functor with the signature
  /// `int visitor(const slice &name, MDBX_db_flags_t flags, const MDBX_stat &stat, MDBX_dbi dbi)`,
  /// which will be called for each table.
  ///
  /// \returns The last value returned by the visitor's functor.
  /// \see ::mdbx_enumerate_tables()
  template <typename VISITOR> inline int enumerate_tables(VISITOR &visitor) const;

  using gc_info = ::MDBX_gc_info_t;
  /// \brief Provides information of Garbage Collection and page usage.
  ///
  /// \details Scans the whole GC to summarise the GC state and page usage for
  /// the given transaction, optionally iterating GC entries by calling the
  /// `visitor` functor for each span of pages inside GC (excepting the pages
  /// forming the B-tree structure of GC itself).
  ///
  /// \param [in,out] visitor  An optional functor with the signature
  /// `int visitor(uint64_t span_txnid, size_t span_pgno, size_t span_length,
  /// bool span_is_reclaimable)` returning \ref continue_loop to proceed or
  /// any other value to stop enumeration.
  ///
  /// \returns The \ref gc_info of the database. An empty (zeroed) info is
  /// returned when the GC is empty (\ref MDBX_NOTFOUND).
  /// \note This API has not been frozen yet.
  /// \see ::mdbx_gc_info()
  template <typename VISITOR> inline gc_info get_gc_info(VISITOR &visitor) const;
  inline gc_info get_gc_info() const;

  using canary = ::MDBX_canary;
  /// \brief Set integers markers (aka "canary") associated with the environment.
  inline txn &put_canary(const canary &);
  /// \brief Returns fours integers markers (aka "canary") associated with the environment.
  inline canary get_canary() const;

  /// Reads sequence generator associated with a key-value map (aka table).
  inline uint64_t sequence(map_handle map) const;
  /// \brief Reads and increment sequence generator associated with a key-value map (aka table).
  inline uint64_t sequence(map_handle map, uint64_t increment);

  /// \brief Compare two keys according to a particular key-value map (aka table).
  inline int compare_keys(map_handle map, const slice &a, const slice &b) const noexcept;
  /// \brief Compare two values according to a particular key-value map (aka table).
  inline int compare_values(map_handle map, const slice &a, const slice &b) const noexcept;
  /// \brief Compare keys of two pairs according to a particular key-value map (aka table).
  inline int compare_keys(map_handle map, const pair &a, const pair &b) const noexcept;
  /// \brief Compare values of two pairs according to a particular key-value map(aka table).
  inline int compare_values(map_handle map, const pair &a, const pair &b) const noexcept;

  /// \brief Get value by key from a key-value map (aka table).
  inline slice get(map_handle map, const slice &key) const;

  using cache_status = ::MDBX_cache_status_t;
  using cache_result = ::MDBX_cache_result_t;

  /// \brief Gets a value by key using the transparent read-your-writes cache.
  ///
  /// \details Uses the cached information to check as quickly as possible
  /// whether the data has changed or not, with early exit when searching
  /// through the DB. Supports multithreaded cases and resolves collisions
  /// in a lockfree way; the \ref MDBX_NOSTICKYTHREADS mode is required to use
  /// it from different threads.
  ///
  /// \param [in] map    The table handle.
  /// \param [in] key    The key to search for.
  /// \param [out] data  The resulting value; points into database-owned
  ///                    memory and remains valid until the transaction end.
  /// \param [in,out] entry  The cache entry corresponding to the key, must be
  ///                    initialized (\ref cache_entry::reset()) before use.
  ///
  /// \returns The \ref cache_result with the pair of error code and cache
  /// status. The caller inspects `errcode` and `status` explicitly.
  /// \see ::mdbx_cache_get()
  MDBX_NODISCARD inline cache_result get_cached(map_handle map, const slice &key, slice *data,
                                                cache_entry &entry) const;

  /// \brief Gets a value by key using the transparent read-your-writes cache,
  /// throwing exceptions on errors.
  ///
  /// \param [in] map    The table handle.
  /// \param [in] key    The key to search for.
  /// \param [in,out] entry  The cache entry corresponding to the key.
  /// \param [out] status  Optional address to receive the \ref cache_status.
  ///
  /// \returns The resulting value, or throws \ref mdbx::not_found if the key
  /// is absent (like \ref get()).
  /// \see ::mdbx_cache_get()
  MDBX_NODISCARD inline slice get_cached(map_handle map, const slice &key, cache_entry &entry,
                                         cache_status *status = nullptr) const;
  /// \brief Get first of multi-value and values count by key from a key-value multimap (aka table).
  inline slice get(map_handle map, slice key, size_t &values_count) const;
  /// \brief Get value by key from a key-value map (aka table).
  inline slice get(map_handle map, const slice &key, const slice &value_at_absence) const;
  /// \brief Get first of multi-value and values count by key from a key-value multimap (aka table).
  inline slice get(map_handle map, slice key, size_t &values_count, const slice &value_at_absence) const;
  /// \brief Get value for equal or great key from a table.
  /// \return Bundle of key-value pair and boolean flag,
  /// which will be `true` if the exact key was found and `false` otherwise.
  inline pair_result get_equal_or_great(map_handle map, const slice &key) const;
  /// \brief Get value for equal or great key from a table.
  /// \return Bundle of key-value pair and boolean flag,
  /// which will be `true` if the exact key was found and `false` otherwise.
  inline pair_result get_equal_or_great(map_handle map, const slice &key, const slice &value_at_absence) const;

  inline MDBX_error_t put(map_handle map, const slice &key, slice *value, MDBX_put_flags_t flags) noexcept;
  inline void put(map_handle map, const slice &key, slice value, put_mode mode);
  inline void insert(map_handle map, const slice &key, slice value);
  inline value_result try_insert(map_handle map, const slice &key, slice value);
  inline slice insert_reserve(map_handle map, const slice &key, size_t value_length);
  inline value_result try_insert_reserve(map_handle map, const slice &key, size_t value_length);

  inline void upsert(map_handle map, const slice &key, const slice &value);
  inline slice upsert_reserve(map_handle map, const slice &key, size_t value_length);

  inline void update(map_handle map, const slice &key, const slice &value);
  inline bool try_update(map_handle map, const slice &key, const slice &value);
  inline slice update_reserve(map_handle map, const slice &key, size_t value_length);
  inline value_result try_update_reserve(map_handle map, const slice &key, size_t value_length);

  void put(map_handle map, const pair &kv, put_mode mode) { return put(map, kv.key, kv.value, mode); }
  void insert(map_handle map, const pair &kv) { return insert(map, kv.key, kv.value); }
  value_result try_insert(map_handle map, const pair &kv) { return try_insert(map, kv.key, kv.value); }
  void upsert(map_handle map, const pair &kv) { return upsert(map, kv.key, kv.value); }

  /// \brief Removes all values for given key.
  inline bool erase(map_handle map, const slice &key);

  /// \brief Removes the particular multi-value entry of the key.
  inline bool erase(map_handle map, const slice &key, const slice &value);

  /// \brief Replaces the particular multi-value of the key with a new value.
  inline void replace(map_handle map, const slice &key, slice &old_value, const slice &new_value);

  /// \brief Removes and return a value of the key.
  template <class BUFFER, class ALLOCATOR = typename BUFFER::allocator_type>
  inline BUFFER extract(map_handle map, const slice &key, const ALLOCATOR &alloc = ALLOCATOR());

  /// \brief Replaces and returns a value of the key with new one.
  template <class BUFFER, class ALLOCATOR = typename BUFFER::allocator_type>
  inline BUFFER replace(map_handle map, const slice &key, const slice &new_value, const ALLOCATOR &alloc = ALLOCATOR());

  template <class BUFFER, class ALLOCATOR = typename BUFFER::allocator_type>
  inline BUFFER replace_reserve(map_handle map, const slice &key, size_t length, slice &new_value_reservation,
                                const ALLOCATOR &alloc = ALLOCATOR());

  /// \brief Adding a key-value pair, provided that ascending order of the keys
  /// and (optionally) values are preserved.
  ///
  /// Instead of splitting the full b+tree pages, the data will be placed on new
  /// ones. Thus appending is about two times faster than insertion, and the
  /// pages will be filled in completely mostly but not half as after splitting
  /// ones. On the other hand, any subsequent insertion or update with an
  /// increase in the length of the value will be twice as slow, since it will
  /// require splitting already filled pages.
  ///
  /// \param [in] map   A map handle to append
  /// \param [in] key   A key to be append
  /// \param [in] value A value to store with the key
  /// \param [in] multivalue_order_preserved
  /// If `multivalue_order_preserved == true` then the same rules applied for
  /// to pages of nested b+tree of multimap's values.
  inline void append(map_handle map, const slice &key, const slice &value, bool multivalue_order_preserved = true);
  inline void append(map_handle map, const pair &kv, bool multivalue_order_preserved = true) {
    return append(map, kv.key, kv.value, multivalue_order_preserved);
  }

  inline size_t put_multiple_samelength(map_handle map, const slice &key, const size_t value_length,
                                        const void *values_array, size_t values_count, put_mode mode,
                                        bool allow_partial = false);
  template <typename VALUE>
  size_t put_multiple_samelength(map_handle map, const slice &key, const VALUE *values_array, size_t values_count,
                                 put_mode mode, bool allow_partial = false) {
    static_assert(::std::is_standard_layout<VALUE>::value && !::std::is_pointer<VALUE>::value &&
                      !::std::is_array<VALUE>::value,
                  "Must be a standard layout type!");
    return put_multiple_samelength(map, key, sizeof(VALUE), values_array, values_count, mode, allow_partial);
  }
  template <typename VALUE>
  void put_multiple_samelength(map_handle map, const slice &key, const ::std::vector<VALUE> &vector, put_mode mode) {
    put_multiple_samelength(map, key, vector.data(), vector.size(), mode);
  }

  inline ptrdiff_t estimate(map_handle map, const pair &from, const pair &to) const;
  inline ptrdiff_t estimate(map_handle map, const slice &from, const slice &to) const;
  inline ptrdiff_t estimate_from_first(map_handle map, const slice &to) const;
  inline ptrdiff_t estimate_to_last(map_handle map, const slice &from) const;
};

/// \brief Managed database transaction.
///
/// As other managed classes, `txn_managed` destroys the represented underlying
/// object from the own class destructor, but disallows copying and assignment
/// for instances.
///
/// All database operations require a transaction handle. Transactions may be
/// read-only or read-write.
class LIBMDBX_API_TYPE txn_managed : public txn {
  using inherited = txn;
  friend class env;
  friend class txn;
  /// delegated constructor for RAII
  MDBX_CXX11_CONSTEXPR txn_managed(MDBX_txn *ptr) noexcept : inherited(ptr) {}

public:
  MDBX_CXX11_CONSTEXPR txn_managed() noexcept = default;
  txn_managed(txn_managed &&) = default;
  txn_managed &operator=(txn_managed &&other) noexcept;
  txn_managed(const txn_managed &) = delete;
  txn_managed &operator=(const txn_managed &) = delete;
  ~txn_managed();

  //----------------------------------------------------------------------------
  using finalization_latency = MDBX_commit_latency;

  /// \brief Abandon all the operations of the transaction instead of saving ones.
  void abort();
  /// \brief Abandon all the operations of the transaction instead of saving ones with collecting latencies information.
  void abort(finalization_latency *);
  /// \brief Abandon all the operations of the transaction instead of saving ones with collecting latencies information.
  void abort(finalization_latency &latency) { return abort(&latency); }
  /// \brief Abandon all the operations of the transaction instead of saving ones with collecting latencies information.
  /// \returns latency information of abort stages.
  finalization_latency abort_get_latency() {
    finalization_latency result;
    abort(&result);
    return result;
  }

  /// \brief Commits all changes of the transaction into a database with collecting latencies information.
  void commit();
  /// \brief Commits all changes of the transaction into a database with collecting latencies information.
  void commit(finalization_latency *);
  /// \brief Commits all changes of the transaction into a database with collecting latencies information.
  void commit(finalization_latency &latency) { return commit(&latency); }
  /// \brief Commits all changes of the transaction into a database and return latency information.
  /// \returns latency information of commit stages.
  finalization_latency commit_get_latency() {
    finalization_latency result;
    commit(&result);
    return result;
  }

  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  bool checkpoint();
  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  bool checkpoint(finalization_latency *latency);
  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  bool checkpoint(finalization_latency &latency) { return checkpoint(&latency); }
  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  /// \returns latency information of commit stages.
  std::pair<bool, finalization_latency> checkpoint_get_latency() {
    finalization_latency latency;
    bool result = checkpoint(&latency);
    return std::make_pair(result, latency);
  }

  /// \brief Commits all the operations of a transaction into the database and then start read transaction.
  void commit_embark_read();
  /// \brief Commits all the operations of a transaction into the database and then start read transaction.
  void commit_embark_read(finalization_latency *latency);
  /// \brief Commits all the operations of a transaction into the database and then start read transaction.
  void commit_embark_read(finalization_latency &latency) { return commit_embark_read(&latency); }
  /// \brief Commits all the operations of a transaction into the database and then start read transaction.
  /// \returns latency information of commit stages.
  finalization_latency commit_embark_read_get_latency() {
    finalization_latency result;
    commit_embark_read(&result);
    return result;
  }

  /// \brief Starts a writing transaction to amending data in the MVCC-snapshot used by the read-only transaction.
  /// \returns The `true` if writing transaction successfully started and `false` if read-only one still continue.
  bool amend(bool dont_wait = false);
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
