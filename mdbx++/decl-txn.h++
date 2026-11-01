// > dist-cutoff-begin
#pragma once
#include "decl-buffer.h++"
#include "decl-core.h++"
#include "decl-env.h++"
#include "decl-exceptions.h++"
#include "decl-slice.h++"
#include "decl-transcoders.h++"
namespace mdbx {
// < dist-cutoff-end

class txn_managed;

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
  MDBX_txn *handle_{nullptr}; ///< \brief The underlying C transaction handle, null for an empty instance.
  /// \brief Constructs a non-owning transaction wrapping the given C handle.
  /// \param ptr  The pointer to the underlying \ref MDBX_txn object.
  MDBX_CXX11_CONSTEXPR txn(MDBX_txn *ptr) noexcept;

public:
  /// \brief Constructs an empty (null) transaction.
  MDBX_CXX11_CONSTEXPR txn() noexcept = default;
  /// \brief Copy constructor.
  txn(const txn &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  txn &operator=(const txn &) noexcept = default;
  /// \brief Move assignment operator.
  /// \param other  The source transaction to be moved from.
  /// \returns A reference to this instance.
  inline txn &operator=(txn &&other) noexcept;
  /// \brief Move constructor: the source transaction is nullified.
  /// \param other  The source transaction to be moved from.
  inline txn(txn &&other) noexcept;
  /// \brief Destroys the wrapper; the underlying object is NOT finished
  /// since the transaction is non-owning.
  inline ~txn() noexcept;
  /* The base handle must never be move-initialized from the owning
   * txn_managed: the move would silently drop ownership, so the transaction
   * (and for a writer, the environment's writer lock) would never be released
   * (issue #77). The exact-match deleted overloads win over the base upcast
   * and turn `mdbx::txn t = env.start_write();` into a compile error.
   * Copying from an lvalue txn_managed stays allowed: the copy is a
   * non-owning view and does not change the source's ownership. */
  txn(txn_managed &&) = delete;
  txn &operator=(txn_managed &&) = delete;

  /// \brief Checks whether the transaction wraps a non-null handle.
  /// \returns `true` if the transaction is not null.
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept { return handle_ != nullptr; };
  /// \brief Returns the underlying C handle (read-only).
  /// \returns The constant pointer to the \ref MDBX_txn object.
  MDBX_CXX14_CONSTEXPR operator const MDBX_txn *() const noexcept { return handle_; }
  /// \brief Returns the underlying C handle.
  /// \returns The mutable pointer to the \ref MDBX_txn object.
  MDBX_CXX14_CONSTEXPR operator MDBX_txn *() noexcept { return handle_; }
  /// \brief Returns the underlying C handle (read-only).
  /// \returns The constant pointer to the \ref MDBX_txn object.
  MDBX_CXX14_CONSTEXPR const MDBX_txn *handle() const noexcept { return handle_; }
  /// \brief Returns the underlying C handle.
  /// \returns The mutable pointer to the \ref MDBX_txn object.
  MDBX_CXX14_CONSTEXPR MDBX_txn *handle() noexcept { return handle_; };
  friend MDBX_CXX11_CONSTEXPR bool operator==(const txn &a, const txn &b) noexcept { return a.handle_ == b.handle_; }
  friend MDBX_CXX11_CONSTEXPR bool operator!=(const txn &a, const txn &b) noexcept { return a.handle_ != b.handle_; }

  /// \brief Returns the transaction's environment.
  /// \returns The \ref env instance the transaction belongs to.
  inline ::mdbx::env env() const noexcept;
  /// \brief Returns transaction's flags.
  /// \returns The \ref MDBX_txn_flags_t combination.
  inline MDBX_txn_flags_t flags() const;
  /// \brief Return the transaction's ID.
  /// \returns The transaction/MVCC-snapshot ID.
  inline uint64_t id() const;

  /// \brief Returns the application context associated with the transaction.
  /// \returns The application context pointer.
  inline void *get_context() const noexcept;

  /// \brief Sets the application context associated with the transaction.
  /// \param your_context  The application context pointer.
  /// \returns The transaction instance.
  inline txn &set_context(void *your_context);

  /// \brief Checks whether the given data is on a dirty page.
  /// \param ptr  The address of the source data.
  /// \returns `true` on success, `false` otherwise.
  inline bool is_dirty(const void *ptr) const;

  /// \brief Checks whether the given slice is on a dirty page.
  /// \param item  The `item` argument.
  /// \returns `true` on success, `false` otherwise.
  inline bool is_dirty(const slice &item) const { return item && is_dirty(item.data()); }

  /// \brief Checks whether the transaction is read-only.
  /// \returns `true` if the transaction is read-only.
  bool is_readonly() const { return (flags() & MDBX_TXN_RDONLY) != 0; }

  /// \brief Checks whether the transaction is read-write.
  /// \returns `true` if the transaction is read-write.
  bool is_readwrite() const { return (flags() & MDBX_TXN_RDONLY) == 0; }

  /// \brief An alias for \ref MDBX_txn_info.
  using info = ::MDBX_txn_info;
  /// \brief Returns information about the MDBX transaction.
  /// \param scan_reader_lock_table  The `scan_reader_lock_table` argument.
  /// \returns Information about the MDBX transaction.
  inline info get_info(bool scan_reader_lock_table = false) const;

  /// \brief Returns maximal write transaction size (i.e. limit for summary
  /// volume of dirty pages) in bytes.
  /// \returns The maximum transaction size in bytes.
  size_t size_max() const { return env().transaction_size_max(); }

  /// \brief Returns current write transaction size (i.e. summary volume of dirty pages) in bytes.
  /// \returns The current transaction size in bytes.
  size_t size_current() const {
    assert(is_readwrite());
    return size_t(get_info().txn_space_dirty);
  }

  //----------------------------------------------------------------------------

  /// \brief Reset read-only transaction.
  inline void reset_reading();

  /// \brief Renew read-only transaction.
  inline void renew_reading();

  /// \brief Refresh the read-only transaction to the most recent snapshot.
  ///
  /// \details Replaces the MVCC-snapshot of the given read-only transaction
  /// with the most recent committed one, see \ref ::mdbx_txn_refresh().
  ///
  /// \returns `True` if the transaction was already reading the most recent
  /// version and no actions were performed, or `false` if it was refreshed
  /// (i.e. advanced to a newer snapshot).
  /// \see reset_reading() \see renew_reading()
  inline bool refresh();

  /// \brief Clone read transaction.
  /// \param context  The application context.
  /// \returns The managed transaction instance.
  inline txn_managed clone(void *context = nullptr) const;

  /// \brief Renew given read transaction into clone.
  /// \param txn_for_renew_into_clone  The `txn_for_renew_into_clone` argument.
  /// \param context  The application context.
  inline void clone(txn_managed &txn_for_renew_into_clone, void *context = nullptr) const;

  /// \brief Copy an MVCC-snapshot of the database to the destination path.
  ///
  /// \details Copies the consistent state as seen by this transaction,
  /// see \ref ::mdbx_txn_copy2pathname().
  /// \see env::copy()
  /// \param destination  The `destination` argument.
  /// \param compactify  The `compactify` argument.
  /// \param force_dynamic_size  The `force_dynamic_size` argument.
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
  /// \param fd  The file descriptor.
  /// \param compactify  The `compactify` argument.
  /// \param force_dynamic_size  The `force_dynamic_size` argument.
  inline void copy(filehandle fd, bool compactify, bool force_dynamic_size = false);

  /// \brief Marks transaction as broken to prevent further operations.
  inline void make_broken();

  /// \brief Park read-only transaction.
  /// \param autounpark  The `autounpark` argument.
  inline void park_reading(bool autounpark = true);

  /// \brief Resume parked read-only transaction.
  /// \returns True if transaction was restarted while `restart_if_ousted=true`.
  /// \param restart_if_ousted  The `restart_if_ousted` argument.
  inline bool unpark_reading(bool restart_if_ousted = true);

  /// \brief Start nested write transaction.
  /// \returns The managed nested write transaction.
  txn_managed start_nested();

  /// \brief Start nested transaction.
  /// \param readonly  Open a read-only transaction.
  /// \returns The managed transaction instance.
  txn_managed start_nested(bool readonly);

  /// \brief Opens cursor for specified key-value map handle.
  /// \param map  The table (map) handle.
  /// \returns The managed cursor instance.
  inline cursor_managed open_cursor(map_handle map) const;

  /// \brief Unbind or close all cursors.
  /// \warning With `unbind=false` it must NOT be used while any cursor of the
  ///   transaction is owned by a \ref cursor_managed instance: the cursor is closed
  ///   behind the wrapper's back, which is left owning a freed handle.
  /// \note With `unbind=true` the wrappers are left alive but detached (see
  ///   \ref unbind_all_cursors()).
  /// \param unbind  Unbind the cursor from the transaction.
  /// \returns The size in bytes.
  inline size_t release_all_cursors(bool unbind) const;

  // TODO: пересмотреть вариант реализации при переходе на двухсвязанные
  // списки курсоров.
  // \brief Close all cursors.
  // Опасный API: освобождает C-хендлы курсоров, которыми могут ещё
  // владеть живые mdbx::cursor_managed (issue #91).
  // inline size_t close_all_cursors() const { return release_all_cursors(false); }

  /// \brief Unbind all cursors.
  /// \details Unbinds (detaches) every live cursor of the transaction, the
  ///   underlying C objects stay allocated and owned. A \ref cursor_managed
  ///   wrapper, however, is not informed about the unbind: it still holds the
  ///   same C handle, so any subsequent cursor operation (seeking, moving,
  ///   reading the current entry, etc.) fails with \ref MDBX_EBADSIGN or
  ///   \ref MDBX_EINVAL at the next C API call. There is no way to detect the
  ///   detached state from the wrapper side except observing those errors.
  ///   To keep using a cursor after this call, re-bind it explicitly via
  ///   \ref cursor::bind() / \ref cursor::renew() (the cursor's `unbind()`
  ///   makes the detachment explicit and is the recommended way to invalidate
  ///   a wrapper you intend to re-use) (issue #146).
  /// \returns The number of unbound cursors.
  inline size_t unbind_all_cursors() const { return release_all_cursors(true); }

  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle open_map(const char *name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const;
  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle open_map(const ::std::string &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const;
  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle open_map(const slice &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const;

  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \returns The table (map) handle.
  inline map_handle open_map_accede(const char *name) const;
  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \returns The table (map) handle.
  inline map_handle open_map_accede(const ::std::string &name) const;
  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \returns The table (map) handle.
  inline map_handle open_map_accede(const slice &name) const;

  /// \brief Create new or open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle create_map(const char *name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single);
  /// \brief Create new or open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle create_map(const ::std::string &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single);
  /// \brief Create new or open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle create_map(const slice &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single);

  /// \brief Drops key-value map using handle.
  /// \param map  The table (map) handle.
  inline void drop_map(map_handle map);
  /// \brief Drops key-value map using name.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  /// \param name  The table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool drop_map(const char *name, bool throw_if_absent = false);
  /// \brief Drop key-value map.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  /// \param name  The table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  inline bool drop_map(const ::std::string &name, bool throw_if_absent = false);
  /// \brief Drop key-value map.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  /// \param name  The table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool drop_map(const slice &name, bool throw_if_absent = false);

  /// \brief Clear key-value map.
  /// \param map  The table (map) handle.
  /// \throws mdbx::error on failure.
  inline void clear_map(map_handle map);
  /// \brief Clear key-value map by name.
  /// \param name             The table name.
  /// \param throw_if_absent  Throw \ref mdbx::not_found if the table does not exist.
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  bool clear_map(const char *name, bool throw_if_absent = false);
  /// \brief Clear key-value map by name.
  /// \param name             The table name.
  /// \param throw_if_absent  Throw \ref mdbx::not_found if the table does not exist.
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  inline bool clear_map(const ::std::string &name, bool throw_if_absent = false);
  /// \brief Clear key-value map by name.
  /// \param name             The table name.
  /// \param throw_if_absent  Throw \ref mdbx::not_found if the table does not exist.
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  bool clear_map(const slice &name, bool throw_if_absent = false);

  /// \brief Переименовывает таблицу ключ-значение.
  /// \param map  The table (map) handle.
  /// \param new_name  The new table name.
  inline void rename_map(map_handle map, const char *new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \param map  The table (map) handle.
  /// \param new_name  The new table name.
  inline void rename_map(map_handle map, const ::std::string &new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \param map  The table (map) handle.
  /// \param new_name  The new table name.
  inline void rename_map(map_handle map, const slice &new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  /// \param old_name  The `old_name` argument.
  /// \param new_name  The new table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool rename_map(const char *old_name, const char *new_name, bool throw_if_absent = false);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  /// \param old_name  The `old_name` argument.
  /// \param new_name  The new table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool rename_map(const ::std::string &old_name, const ::std::string &new_name, bool throw_if_absent = false);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  /// \param old_name  The `old_name` argument.
  /// \param new_name  The new table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool rename_map(const slice &old_name, const slice &new_name, bool throw_if_absent = false);

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)

  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle open_map(const ::std::string_view &name, const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                             const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) const {
    return open_map(slice(name), key_mode, value_mode);
  }
  /// \brief Open existing key-value map.
  /// \param name  The table name.
  /// \returns The table (map) handle.
  inline map_handle open_map_accede(const ::std::string_view &name) const;
  /// \brief Create new or open existing key-value map.
  /// \param name  The table name.
  /// \param key_mode  The key comparison mode, see \ref mdbx::key_mode.
  /// \param value_mode  The value comparison mode, see \ref mdbx::value_mode.
  /// \returns The table (map) handle.
  inline map_handle create_map(const ::std::string_view &name,
                               const ::mdbx::key_mode key_mode = ::mdbx::key_mode::usual,
                               const ::mdbx::value_mode value_mode = ::mdbx::value_mode::single) {
    return create_map(slice(name), key_mode, value_mode);
  }
  /// \brief Drop key-value map.
  /// \return `True` if the key-value map existed and was deleted, either
  /// `false` if the key-value map did not exist and there is nothing to delete.
  /// \param name  The table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool drop_map(const ::std::string_view &name, bool throw_if_absent = false) {
    return drop_map(slice(name), throw_if_absent);
  }
  /// \brief Clear key-value map by name.
  /// \param name             The table name.
  /// \param throw_if_absent  Throw \ref mdbx::not_found if the table does not exist.
  /// \return `True` if the key-value map existed and was cleared, either
  /// `false` if the key-value map did not exist and there is nothing to clear.
  bool clear_map(const ::std::string_view &name, bool throw_if_absent = false) {
    return clear_map(slice(name), throw_if_absent);
  }
  /// \brief Переименовывает таблицу ключ-значение.
  /// \param map  The table (map) handle.
  /// \param new_name  The new table name.
  inline void rename_map(map_handle map, const ::std::string_view &new_name);
  /// \brief Переименовывает таблицу ключ-значение.
  /// \return `True` если таблица существует и была переименована, либо
  /// `false` в случае отсутствия исходной таблицы.
  /// \param old_name  The `old_name` argument.
  /// \param new_name  The new table name.
  /// \param throw_if_absent  The `throw_if_absent` argument.
  bool rename_map(const ::std::string_view &old_name, const ::std::string_view &new_name,
                  bool throw_if_absent = false) {
    return rename_map(slice(old_name), slice(new_name), throw_if_absent);
  }
#endif /* __cpp_lib_string_view >= 201606L */

  /// \brief An alias for \ref MDBX_stat, i.e. the table statistics.
  using map_stat = ::MDBX_stat;
  /// \brief Returns statistics for a table.
  /// \param map  The table (map) handle.
  /// \returns Statistics for a table.
  inline map_stat get_map_stat(map_handle map) const;
  /// \brief Returns depth (bitmask) information of nested dupsort (multi-value)
  /// B+trees for given table.
  /// \param map  The table (map) handle.
  /// \returns Depth (bitmask) information of nested dupsort (multi-value) B+trees for given table.
  inline uint32_t get_tree_deepmask(map_handle map) const;
  /// \brief Returns information about key-value map (aka table) handle.
  /// \param map  The table (map) handle.
  /// \returns Information about key-value map (aka table) handle.
  inline map_handle::info get_map_flags(map_handle map) const;

  /// \brief Enumerates user's named tables in a database.
  ///
  /// \details Calls the `visitor` functor for each user-created named table
  /// until the named tables are exhausted, or until the visitor returns
  /// `exit_loop` (which will be returned as a result).
  ///
  /// \param [in,out] visitor  A functor with the signature
  /// `int visitor(const slice &name, MDBX_db_flags_t flags, const MDBX_stat &stat, MDBX_dbi dbi)`,
  /// which will be called for each table.
  ///
  /// \returns The last value returned by the visitor's functor.
  /// \see ::mdbx_enumerate_tables()
  template <typename VISITOR> inline int enumerate_tables(VISITOR &visitor) const;

  /// \brief An alias for \ref MDBX_gc_info_t, i.e. the garbage collection info.
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
  /// bool span_is_reclaimable)` returning `continue_loop` to proceed or
  /// any other value to stop enumeration.
  ///
  /// \returns The \ref gc_info of the database. An empty (zeroed) info is
  /// returned when the GC is empty (\ref MDBX_NOTFOUND).
  /// \note This API has not been frozen yet.
  /// \see ::mdbx_gc_info()
  template <typename VISITOR> inline gc_info get_gc_info(VISITOR &visitor) const;
  /// \brief Returns information about garbage collection and page usage.
  /// \returns The \ref gc_info for the current transaction.
  inline gc_info get_gc_info() const;

  /// \brief An alias for \ref MDBX_canary.
  using canary = ::MDBX_canary;
  /// \brief Set integers markers (aka "canary") associated with the environment.
  /// \param canary  The canary markers to set.
  /// \returns A reference to this transaction.
  inline txn &put_canary(const canary &);
  /// \brief Returns the four integer markers (aka "canary") associated with the environment.
  /// \returns The canary markers.
  inline canary get_canary() const;

  /// \brief Reads sequence generator associated with a key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \returns The current sequence value.
  inline uint64_t sequence(map_handle map) const;
  /// \brief Reads and increment sequence generator associated with a key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \param increment  The increment of the sequence.
  /// \returns The resulting value.
  inline uint64_t sequence(map_handle map, uint64_t increment);

  /// \brief Compare two keys according to a particular key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \param a  The left-hand operand.
  /// \param b  The right-hand operand.
  /// \returns The resulting value.
  inline int compare_keys(map_handle map, const slice &a, const slice &b) const noexcept;
  /// \brief Compare two values according to a particular key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \param a  The left-hand operand.
  /// \param b  The right-hand operand.
  /// \returns The resulting value.
  inline int compare_values(map_handle map, const slice &a, const slice &b) const noexcept;
  /// \brief Compare keys of two pairs according to a particular key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \param a  The left-hand operand.
  /// \param b  The right-hand operand.
  /// \returns The resulting value.
  inline int compare_keys(map_handle map, const pair &a, const pair &b) const noexcept;
  /// \brief Compare values of two pairs according to a particular key-value map(aka table).
  /// \param map  The table (map) handle.
  /// \param a  The left-hand operand.
  /// \param b  The right-hand operand.
  /// \returns The resulting value.
  inline int compare_values(map_handle map, const pair &a, const pair &b) const noexcept;

  /// \brief Get value by key from a key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \returns The resulting slice.
  inline slice get(map_handle map, const slice &key) const;

  /// \brief An alias for \ref MDBX_cache_status_t, i.e. the cache lookup status.
  using cache_status = ::MDBX_cache_status_t;
  /// \brief An alias for \ref MDBX_cache_result_t, i.e. the cached lookup result.
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
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param values_count  The number of values in the array.
  /// \returns The resulting slice.
  inline slice get(map_handle map, slice key, size_t &values_count) const;
  /// \brief Get value by key from a key-value map (aka table).
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param value_at_absence  The `value_at_absence` argument.
  /// \returns The resulting slice.
  inline slice get(map_handle map, const slice &key, const slice &value_at_absence) const;
  /// \brief Get first of multi-value and values count by key from a key-value multimap (aka table).
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param values_count  The number of values in the array.
  /// \param value_at_absence  The `value_at_absence` argument.
  /// \returns The resulting slice.
  inline slice get(map_handle map, slice key, size_t &values_count, const slice &value_at_absence) const;
  /// \brief Gets the value for an equal or greater key from a table.
  /// \return Bundle of key-value pair and boolean flag,
  /// which will be `true` if the exact key was found and `false` otherwise.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  inline pair_result get_equal_or_great(map_handle map, const slice &key) const;
  /// \brief Gets the value for an equal or greater key from a table.
  /// \return Bundle of key-value pair and boolean flag,
  /// which will be `true` if the exact key was found and `false` otherwise.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param value_at_absence  The `value_at_absence` argument.
  inline pair_result get_equal_or_great(map_handle map, const slice &key, const slice &value_at_absence) const;

  /// \brief Stores a key-value pair using the given low-level flags.
  /// \param map    The table (map) handle.
  /// \param key    The key to store.
  /// \param value  The value to store (in/out for some flags).
  /// \param flags  The \ref MDBX_put_flags_t combination.
  /// \returns An error code, zero on success (does not throw).
  inline MDBX_error_t put(map_handle map, const slice &key, slice *value, MDBX_put_flags_t flags) noexcept;
  /// \brief Stores a key-value pair in the given mode.
  /// \param map    The table (map) handle.
  /// \param key    The key to store.
  /// \param value  The value to store.
  /// \param mode   The \ref put_mode (insert/upsert/update).
  /// \throws mdbx::error on failure.
  inline void put(map_handle map, const slice &key, slice value, put_mode mode);
  /// \brief Inserts a new key-value pair; fails if the key already exists.
  /// \param map    The table (map) handle.
  /// \param key    The key to insert.
  /// \param value  The value to insert.
  /// \throws mdbx::key_exists if the key already exists.
  inline void insert(map_handle map, const slice &key, slice value);
  /// \brief Tries to insert a new key-value pair without throwing on a duplicate.
  /// \param map    The table (map) handle.
  /// \param key    The key to insert.
  /// \param value  The value to insert.
  /// \returns The \ref value_result with the success flag.
  inline value_result try_insert(map_handle map, const slice &key, slice value);
  /// \brief Reserves space for a value of the given length for a new key.
  /// \param map           The table (map) handle.
  /// \param key           The key to insert.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  /// \throws mdbx::key_exists if the key already exists.
  inline slice insert_reserve(map_handle map, const slice &key, size_t value_length);
  /// \brief Tries to reserve space for a value of the given length for a new key.
  /// \param map           The table (map) handle.
  /// \param key           The key to insert.
  /// \param value_length  The length of the value to reserve.
  /// \returns The \ref value_result with the reserved slice and the success flag.
  inline value_result try_insert_reserve(map_handle map, const slice &key, size_t value_length);

  /// \brief Inserts or updates the value for the given key.
  /// \param map    The table (map) handle.
  /// \param key    The key to store.
  /// \param value  The value to store.
  /// \throws mdbx::error on failure.
  inline void upsert(map_handle map, const slice &key, const slice &value);
  /// \brief Reserves space for a value of the given length, inserting or updating the key.
  /// \param map           The table (map) handle.
  /// \param key           The key to store.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  inline slice upsert_reserve(map_handle map, const slice &key, size_t value_length);

  /// \brief Updates the value for an existing key; fails if the key is absent.
  /// \param map    The table (map) handle.
  /// \param key    The key to update.
  /// \param value  The new value.
  /// \throws mdbx::not_found if the key does not exist.
  inline void update(map_handle map, const slice &key, const slice &value);
  /// \brief Tries to update the value for an existing key without throwing.
  /// \param map    The table (map) handle.
  /// \param key    The key to update.
  /// \param value  The new value.
  /// \returns `true` if the key was found and updated.
  inline bool try_update(map_handle map, const slice &key, const slice &value);
  /// \brief Reserves space for a value of the given length for an existing key.
  /// \param map           The table (map) handle.
  /// \param key           The key to update.
  /// \param value_length  The length of the value to reserve.
  /// \returns The writable slice for the reserved value.
  /// \throws mdbx::not_found if the key does not exist.
  inline slice update_reserve(map_handle map, const slice &key, size_t value_length);
  /// \brief Tries to reserve space for a value of the given length for an existing key.
  /// \param map           The table (map) handle.
  /// \param key           The key to update.
  /// \param value_length  The length of the value to reserve.
  /// \returns The \ref value_result with the reserved slice and the success flag.
  inline value_result try_update_reserve(map_handle map, const slice &key, size_t value_length);

  /// \brief Stores a key-value pair in the given mode.
  /// \param map   The table (map) handle.
  /// \param kv    The key-value pair.
  /// \param mode  The \ref put_mode (insert/upsert/update).
  void put(map_handle map, const pair &kv, put_mode mode) { return put(map, kv.key, kv.value, mode); }
  /// \brief Inserts a new key-value pair; fails if the key already exists.
  /// \param map  The table (map) handle.
  /// \param kv   The key-value pair to insert.
  void insert(map_handle map, const pair &kv) { return insert(map, kv.key, kv.value); }
  /// \brief Tries to insert a new key-value pair without throwing on a duplicate.
  /// \param map  The table (map) handle.
  /// \param kv   The key-value pair to insert.
  /// \returns The \ref value_result with the success flag.
  value_result try_insert(map_handle map, const pair &kv) { return try_insert(map, kv.key, kv.value); }
  /// \brief Inserts or updates the value for the given key.
  /// \param map  The table (map) handle.
  /// \param kv   The key-value pair to store.
  void upsert(map_handle map, const pair &kv) { return upsert(map, kv.key, kv.value); }

  /// \brief Removes all values for given key.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \returns `true` on success, `false` otherwise.
  inline bool erase(map_handle map, const slice &key);

  /// \brief Removes the particular multi-value entry of the key.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param value  The value.
  /// \returns `true` on success, `false` otherwise.
  inline bool erase(map_handle map, const slice &key, const slice &value);

  /// \brief Replaces the particular multi-value of the key with a new value.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param old_value  The `old_value` argument.
  /// \param new_value  The `new_value` argument.
  inline void replace(map_handle map, const slice &key, slice &old_value, const slice &new_value);

  /// \brief Removes and return a value of the key.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param alloc  The allocator for the buffer storage.
  /// \returns The result.
  template <class BUFFER, class ALLOCATOR = typename BUFFER::allocator_type>
  inline BUFFER extract(map_handle map, const slice &key, const ALLOCATOR &alloc = ALLOCATOR());

  /// \brief Replaces and returns a value of the key with new one.
  /// \param map  The table (map) handle.
  /// \param key  The key.
  /// \param new_value  The `new_value` argument.
  /// \param alloc  The allocator for the buffer storage.
  /// \returns The result.
  template <class BUFFER, class ALLOCATOR = typename BUFFER::allocator_type>
  inline BUFFER replace(map_handle map, const slice &key, const slice &new_value, const ALLOCATOR &alloc = ALLOCATOR());

  /// \brief Replaces the value of the key reserving space for the new one, and
  /// returns the old value as a buffer.
  /// \param map                     The table (map) handle.
  /// \param key                     The key to replace.
  /// \param length                  The length of the new value to reserve.
  /// \param new_value_reservation   Out: the writable slice for the reserved new value.
  /// \param alloc                   The allocator for the returned buffer.
  /// \returns The buffer with the old value (empty if the key was absent).
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
  /// \brief Appends a key-value pair, provided that the ascending order is preserved.
  /// \param map                         The table (map) handle.
  /// \param kv                          The key-value pair to append.
  /// \param multivalue_order_preserved  The same rules apply to the nested multi-value tree.
  inline void append(map_handle map, const pair &kv, bool multivalue_order_preserved = true) {
    return append(map, kv.key, kv.value, multivalue_order_preserved);
  }

  /// \brief Stores a bunch of same-length multi-values for the given key.
  /// \param map            The table (map) handle.
  /// \param key            The key of the multi-value hive.
  /// \param value_length   The length of each value in bytes.
  /// \param values_array   The array of values.
  /// \param values_count   The number of values in the array.
  /// \param mode           The \ref put_mode.
  /// \param allow_partial  Allow a partial store when some values are rejected.
  /// \returns The number of stored values.
  inline size_t put_multiple_samelength(map_handle map, const slice &key, const size_t value_length,
                                        const void *values_array, size_t values_count, put_mode mode,
                                        bool allow_partial = false);
  /// \brief Stores a bunch of same-length multi-values for the given key.
  /// \param map            The table (map) handle.
  /// \param key            The key of the multi-value hive.
  /// \param values_array   The array of standard-layout values.
  /// \param values_count   The number of values in the array.
  /// \param mode           The \ref put_mode.
  /// \param allow_partial  Allow a partial store when some values are rejected.
  /// \returns The number of stored values.
  template <typename VALUE>
  size_t put_multiple_samelength(map_handle map, const slice &key, const VALUE *values_array, size_t values_count,
                                 put_mode mode, bool allow_partial = false) {
    static_assert(::std::is_standard_layout<VALUE>::value && !::std::is_pointer<VALUE>::value &&
                      !::std::is_array<VALUE>::value,
                  "Must be a standard layout type!");
    return put_multiple_samelength(map, key, sizeof(VALUE), values_array, values_count, mode, allow_partial);
  }
  /// \brief Stores a bunch of same-length multi-values for the given key.
  /// \param map     The table (map) handle.
  /// \param key     The key of the multi-value hive.
  /// \param vector  The vector of standard-layout values.
  /// \param mode    The \ref put_mode.
  template <typename VALUE>
  void put_multiple_samelength(map_handle map, const slice &key, const ::std::vector<VALUE> &vector, put_mode mode) {
    put_multiple_samelength(map, key, vector.data(), vector.size(), mode);
  }

  /// \brief Estimates the number of items between two key-value pairs.
  /// \param map   The table (map) handle.
  /// \param from  The "from" boundary pair.
  /// \param to    The "to" boundary pair.
  /// \returns The approximate distance; a negative value means `to` is before `from`.
  inline ptrdiff_t estimate(map_handle map, const pair &from, const pair &to) const;
  /// \brief Estimates the number of items between two keys.
  /// \param map   The table (map) handle.
  /// \param from  The "from" boundary key.
  /// \param to    The "to" boundary key.
  /// \returns The approximate distance; a negative value means `to` is before `from`.
  inline ptrdiff_t estimate(map_handle map, const slice &from, const slice &to) const;
  /// \brief Estimates the number of items from the first one up to the given key.
  /// \param map  The table (map) handle.
  /// \param to   The "to" boundary key.
  /// \returns The approximate distance.
  inline ptrdiff_t estimate_from_first(map_handle map, const slice &to) const;
  /// \brief Estimates the number of items from the given key up to the last one.
  /// \param map   The table (map) handle.
  /// \param from  The "from" boundary key.
  /// \returns The approximate distance.
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
  /// \brief Move constructor.
  txn_managed(txn_managed &&) = default;
  /// \brief Move assignment operator.
  /// \param other  The source managed transaction to be moved from.
  /// \returns A reference to this instance.
  txn_managed &operator=(txn_managed &&other) noexcept;
  txn_managed(const txn_managed &) = delete;
  txn_managed &operator=(const txn_managed &) = delete;
  ~txn_managed();

  //----------------------------------------------------------------------------
  /// \brief An alias for \ref MDBX_commit_latency, i.e. the commit latency info.
  using finalization_latency = MDBX_commit_latency;

  /// \brief Abandon all the operations of the transaction instead of saving ones.
  void abort();
  /// \brief Abandon all the operations of the transaction instead of saving ones with collecting latencies information.
  void abort(finalization_latency *);
  /// \brief Abandon all the operations of the transaction instead of saving ones with collecting latencies information.
  /// \param latency  The latency structure to fill.
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
  /// \param latency  The latency structure to fill.
  void commit(finalization_latency &latency) { return commit(&latency); }
  /// \brief Commits all changes of the transaction into a database and return latency information.
  /// \returns latency information of commit stages.
  finalization_latency commit_get_latency() {
    finalization_latency result;
    commit(&result);
    return result;
  }

  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  /// \returns `true` on success, `false` otherwise.
  bool checkpoint();
  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  /// \param latency  The latency structure to fill.
  /// \returns `true` on success, `false` otherwise.
  bool checkpoint(finalization_latency *latency);
  /// \brief Commits all the operations of the transaction and immediately starts next without releasing any locks.
  /// \param latency  The latency structure to fill.
  /// \returns `true` on success, `false` otherwise.
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
  /// \param latency  The latency structure to fill.
  void commit_embark_read(finalization_latency *latency);
  /// \brief Commits all the operations of a transaction into the database and then start read transaction.
  /// \param latency  The latency structure to fill.
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
  /// \param dont_wait  Don't wait if the resource is locked by another thread.
  bool amend(bool dont_wait = false);
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
