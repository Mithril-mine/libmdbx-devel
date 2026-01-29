// > dist-cutoff-begin
#pragma once
#include "decl_buffer.h++"
#include "decl_core.h++"
#include "decl_exceptions.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief Unmanaged database environment.
///
/// Like other unmanaged classes, `env` allows copying and assignment for handles as a values,
/// but does not manage nor destroys the represented underlying object from the own class destructor.
///
/// An environment supports multiple key-value tables (aka key-value maps,
/// tables or sub-databases), all residing in the same shared-memory mapped file.
class LIBMDBX_API_TYPE env {
  friend class txn;

protected:
  MDBX_env *handle_{nullptr};
  MDBX_CXX11_CONSTEXPR env(MDBX_env *ptr) noexcept;

public:
  MDBX_CXX11_CONSTEXPR env() noexcept = default;
  env(const env &) noexcept = default;
  env &operator=(const env &) noexcept = default;
  inline env &operator=(env &&) noexcept;
  inline env(env &&) noexcept;
  inline ~env() noexcept;

  MDBX_CXX14_CONSTEXPR operator bool() const noexcept;
  MDBX_CXX14_CONSTEXPR operator const MDBX_env *() const;
  MDBX_CXX14_CONSTEXPR operator MDBX_env *();
  friend MDBX_CXX11_CONSTEXPR bool operator==(const env &a, const env &b) noexcept;
  friend MDBX_CXX11_CONSTEXPR bool operator!=(const env &a, const env &b) noexcept;

  //----------------------------------------------------------------------------

  /// \brief Database geometry for size management.
  /// \see env_managed::create_parameters
  /// \see env_managed::env_managed(const ::std::string &pathname, const
  /// create_parameters &, const operate_parameters &, bool accede)

  struct LIBMDBX_API_TYPE geometry {
    enum : intptr_t {
      default_value = -1,         ///< Means "keep current or use default"
      minimal_value = 0,          ///< Means "minimal acceptable"
      maximal_value = INTPTR_MAX, ///< Means "maximal acceptable"
      kB = 1000,                  ///< \f$10^{3}\f$ bytes (0x03E8)
      MB = kB * 1000,             ///< \f$10^{6}\f$ bytes (0x000F_4240)
      GB = MB * 1000,             ///< \f$10^{9}\f$ bytes (0x3B9A_CA00)
#if INTPTR_MAX > 0x7fffFFFFl
      TB = GB * 1000,  ///< \f$10^{12}\f$ bytes (0x0000_00E8_D4A5_1000)
      PB = TB * 1000,  ///< \f$10^{15}\f$ bytes (0x0003_8D7E_A4C6_8000)
      EB = PB * 1000,  ///< \f$10^{18}\f$ bytes (0x0DE0_B6B3_A764_0000)
#endif                 /* 64-bit intptr_t */
      KiB = 1024,      ///< \f$2^{10}\f$ bytes (0x0400)
      MiB = KiB << 10, ///< \f$2^{20}\f$ bytes (0x0010_0000)
      GiB = MiB << 10, ///< \f$2^{30}\f$ bytes (0x4000_0000)
#if INTPTR_MAX > 0x7fffFFFFl
      TiB = GiB << 10, ///< \f$2^{40}\f$ bytes (0x0000_0100_0000_0000)
      PiB = TiB << 10, ///< \f$2^{50}\f$ bytes (0x0004_0000_0000_0000)
      EiB = PiB << 10, ///< \f$2^{60}\f$ bytes (0x1000_0000_0000_0000)
#endif                 /* 64-bit intptr_t */
    };

    /// \brief Tagged type for output to std::ostream
    struct size {
      intptr_t bytes;
      MDBX_CXX11_CONSTEXPR size(intptr_t bytes) noexcept : bytes(bytes) {}
      MDBX_CXX11_CONSTEXPR operator intptr_t() const noexcept { return bytes; }
    };

    /// \brief The lower bound of database size in bytes.
    intptr_t size_lower{default_value};

    /// \brief The size in bytes to setup the database size for now.
    /// \details It is recommended always pass \ref default_value in this
    /// argument except some special cases.
    intptr_t size_now{default_value};

    /// \brief The upper bound of database size in bytes.
    /// \details It is recommended to avoid change upper bound while database is
    /// used by other processes or threaded (i.e. just pass \ref default_value
    /// in this argument except absolutely necessary). Otherwise you must be
    /// ready for \ref MDBX_UNABLE_EXTEND_MAPSIZE error(s), unexpected pauses
    /// during remapping and/or system errors like "address busy", and so on. In
    /// other words, there is no way to handle a growth of the upper bound
    /// robustly because there may be a lack of appropriate system resources
    /// (which are extremely volatile in a multi-process multi-threaded
    /// environment).
    intptr_t size_upper{default_value};

    /// \brief The growth step in bytes, must be greater than zero to allow the database to grow.
    intptr_t growth_step{default_value};

    /// \brief The shrink threshold in bytes, must be greater than zero to allow the database to shrink.
    intptr_t shrink_threshold{default_value};

    /// \brief The database page size for new database creation
    /// or \ref default_value otherwise.
    /// \details Must be power of 2 in the range between \ref MDBX_MIN_PAGESIZE
    /// and \ref MDBX_MAX_PAGESIZE.
    intptr_t pagesize{default_value};

    inline geometry &make_fixed(intptr_t size) noexcept;
    inline geometry &make_dynamic(intptr_t lower = default_value, intptr_t upper = default_value) noexcept;
    MDBX_CXX11_CONSTEXPR geometry() noexcept {}
    MDBX_CXX11_CONSTEXPR
    geometry(const geometry &) noexcept = default;
    MDBX_CXX11_CONSTEXPR geometry(intptr_t size_lower, intptr_t size_now = default_value,
                                  intptr_t size_upper = default_value, intptr_t growth_step = default_value,
                                  intptr_t shrink_threshold = default_value, intptr_t pagesize = default_value) noexcept
        : size_lower(size_lower), size_now(size_now), size_upper(size_upper), growth_step(growth_step),
          shrink_threshold(shrink_threshold), pagesize(pagesize) {}
  };

  /// \brief Operation mode.
  enum mode {
    readonly,        ///< \copydoc MDBX_RDONLY
    write_file_io,   // don't available on OpenBSD
    write_mapped_io, ///< \copydoc MDBX_WRITEMAP
    nested_transactions = write_file_io
  };

  /// \brief Durability level.
  enum durability {
    robust_synchronous,         ///< \copydoc MDBX_SYNC_DURABLE
    half_synchronous_weak_last, ///< \copydoc MDBX_NOMETASYNC
    lazy_weak_tail,             ///< \copydoc MDBX_SAFE_NOSYNC
    whole_fragile               ///< \copydoc MDBX_UTTERLY_NOSYNC
  };

  /// \brief Garbage reclaiming options.
  struct LIBMDBX_API_TYPE reclaiming_options {
    /// \copydoc MDBX_LIFORECLAIM
    bool lifo{false};
    /// \copydoc MDBX_COALESCE
    bool coalesce{false};
    MDBX_CXX11_CONSTEXPR reclaiming_options() noexcept {}
    MDBX_CXX11_CONSTEXPR
    reclaiming_options(const reclaiming_options &) noexcept = default;
    MDBX_CXX14_CONSTEXPR reclaiming_options &operator=(const reclaiming_options &) noexcept = default;
    reclaiming_options(MDBX_env_flags_t) noexcept;
  };

  /// \brief Operate options.
  struct LIBMDBX_API_TYPE operate_options {
    /// \copydoc MDBX_NOSTICKYTHREADS
    bool no_sticky_threads{false};
    /// \brief Разрешает вложенные транзакции ценой отключения
    /// \ref MDBX_WRITEMAP и увеличением накладных расходов.
    bool nested_transactions{false};
    /// \copydoc MDBX_EXCLUSIVE
    bool exclusive{false};
    /// \copydoc MDBX_NORDAHEAD
    bool disable_readahead{false};
    /// \copydoc MDBX_NOMEMINIT
    bool disable_clear_memory{false};
    /// \copydoc MDBX_VALIDATION
    bool enable_validation{false};
    MDBX_CXX11_CONSTEXPR operate_options() noexcept {}
    MDBX_CXX11_CONSTEXPR
    operate_options(const operate_options &) noexcept = default;
    MDBX_CXX14_CONSTEXPR operate_options &operator=(const operate_options &) noexcept = default;
    operate_options(MDBX_env_flags_t) noexcept;
  };

  /// \brief Operate parameters.
  struct LIBMDBX_API_TYPE operate_parameters {
    /// \brief The maximum number of named tables/maps for the environment.
    /// Zero means default value.
    unsigned max_maps{0};
    /// \brief The maximum number of threads/reader slots for the environment.
    /// Zero means default value.
    unsigned max_readers{0};
    env::mode mode{write_mapped_io};
    env::durability durability{robust_synchronous};
    env::reclaiming_options reclaiming;
    env::operate_options options;

    MDBX_CXX11_CONSTEXPR operate_parameters() noexcept {}
    MDBX_CXX11_CONSTEXPR
    operate_parameters(const unsigned max_maps, const unsigned max_readers = 0,
                       const env::mode mode = env::mode::write_mapped_io,
                       env::durability durability = env::durability::robust_synchronous,
                       const env::reclaiming_options &reclaiming = env::reclaiming_options(),
                       const env::operate_options &options = env::operate_options()) noexcept
        : max_maps(max_maps), max_readers(max_readers), mode(mode), durability(durability), reclaiming(reclaiming),
          options(options) {}
    MDBX_CXX11_CONSTEXPR
    operate_parameters(const operate_parameters &) noexcept = default;
    MDBX_CXX14_CONSTEXPR operate_parameters &operator=(const operate_parameters &) noexcept = default;
    MDBX_env_flags_t make_flags(bool accede = true,           ///< Allows accepting incompatible operating options
                                                              ///< in case the database is already being used by
                                                              ///< another process(es) \see MDBX_ACCEDE
                                bool use_subdirectory = false ///< use subdirectory to place the DB files
    ) const;
    static env::mode mode_from_flags(MDBX_env_flags_t) noexcept;
    static env::durability durability_from_flags(MDBX_env_flags_t) noexcept;
    inline static env::reclaiming_options reclaiming_from_flags(MDBX_env_flags_t flags) noexcept;
    inline static env::operate_options options_from_flags(MDBX_env_flags_t flags) noexcept;
  };

  /// \brief Returns current operation parameters.
  inline env::operate_parameters get_operation_parameters() const;
  /// \brief Returns current operation mode.
  inline env::mode get_mode() const;
  /// \brief Returns current durability mode.
  inline env::durability get_durability() const;
  /// \brief Returns current reclaiming options.
  inline env::reclaiming_options get_reclaiming() const;
  /// \brief Returns current operate options.
  inline env::operate_options get_options() const;

  /// \brief Returns `true` for a freshly created database,
  /// but `false` if at least one transaction was committed.
  bool is_pristine() const;

  /// \brief Checks whether the database is empty.
  bool is_empty() const;

  /// \brief Returns default page size for current system/platform.
  static size_t default_pagesize() noexcept { return ::mdbx_default_pagesize(); }

  struct limits {
    limits() = delete;
    /// \brief Returns the minimal database page size in bytes.
    static inline size_t pagesize_min() noexcept;
    /// \brief Returns the maximal database page size in bytes.
    static inline size_t pagesize_max() noexcept;
    /// \brief Returns the minimal database size in bytes for specified page size.
    static inline size_t dbsize_min(intptr_t pagesize);
    /// \brief Returns the maximal database size in bytes for specified page size.
    static inline size_t dbsize_max(intptr_t pagesize);
    /// \brief Returns the minimal key size in bytes for specified table flags.
    static inline size_t key_min(MDBX_db_flags_t flags) noexcept;
    /// \brief Returns the minimal key size in bytes for specified keys mode.
    static inline size_t key_min(key_mode mode) noexcept;
    /// \brief Returns the maximal key size in bytes for specified page size and table flags.
    static inline size_t key_max(intptr_t pagesize, MDBX_db_flags_t flags);
    /// \brief Returns the maximal key size in bytes for specified page size and keys mode.
    static inline size_t key_max(intptr_t pagesize, key_mode mode);
    /// \brief Returns the maximal key size in bytes for given environment and table flags.
    static inline size_t key_max(const env &, MDBX_db_flags_t flags);
    /// \brief Returns the maximal key size in bytes for given environment and keys mode.
    static inline size_t key_max(const env &, key_mode mode);
    /// \brief Returns the minimal values size in bytes for specified table flags.
    static inline size_t value_min(MDBX_db_flags_t flags) noexcept;
    /// \brief Returns the minimal values size in bytes for specified values mode.
    static inline size_t value_min(value_mode) noexcept;

    /// \brief Returns the maximal value size in bytes for specified page size and table flags.
    static inline size_t value_max(intptr_t pagesize, MDBX_db_flags_t flags);
    /// \brief Returns the maximal value size in bytes for specified page size and values mode.
    static inline size_t value_max(intptr_t pagesize, value_mode);
    /// \brief Returns the maximal value size in bytes for given environment and table flags.
    static inline size_t value_max(const env &, MDBX_db_flags_t flags);
    /// \brief Returns the maximal value size in bytes for specified page size and values mode.
    static inline size_t value_max(const env &, value_mode);

    /// \brief Returns maximal size of key-value pair to fit in a single page
    /// for specified size and table flags.
    static inline size_t pairsize4page_max(intptr_t pagesize, MDBX_db_flags_t flags);
    /// \brief Returns maximal size of key-value pair to fit in a single page
    /// for specified page size and values mode.
    static inline size_t pairsize4page_max(intptr_t pagesize, value_mode);
    /// \brief Returns maximal size of key-value pair to fit in a single page
    /// for given environment and table flags.
    static inline size_t pairsize4page_max(const env &, MDBX_db_flags_t flags);
    /// \brief Returns maximal size of key-value pair to fit in a single page
    /// for specified page size and values mode.
    static inline size_t pairsize4page_max(const env &, value_mode);

    /// \brief Returns maximal data size in bytes to fit in a leaf-page or
    /// single large/overflow-page for specified size and table flags.
    static inline size_t valsize4page_max(intptr_t pagesize, MDBX_db_flags_t flags);
    /// \brief Returns maximal data size in bytes to fit in a leaf-page or
    /// single large/overflow-page for specified page size and values mode.
    static inline size_t valsize4page_max(intptr_t pagesize, value_mode);
    /// \brief Returns maximal data size in bytes to fit in a leaf-page or
    /// single large/overflow-page for given environment and table flags.
    static inline size_t valsize4page_max(const env &, MDBX_db_flags_t flags);
    /// \brief Returns maximal data size in bytes to fit in a leaf-page or
    /// single large/overflow-page for specified page size and values mode.
    static inline size_t valsize4page_max(const env &, value_mode);

    /// \brief Returns the maximal write transaction size (i.e. limit for
    /// summary volume of dirty pages) in bytes for specified page size.
    static inline size_t transaction_size_max(intptr_t pagesize);

    /// \brief Returns the maximum opened map handles, aka DBI-handles.
    static inline size_t max_map_handles(void);
  };

  /// \brief Returns the minimal database size in bytes for the environment.
  size_t dbsize_min() const { return limits::dbsize_min(this->get_pagesize()); }
  /// \brief Returns the maximal database size in bytes for the environment.
  size_t dbsize_max() const { return limits::dbsize_max(this->get_pagesize()); }
  /// \brief Returns the minimal key size in bytes for specified keys mode.
  size_t key_min(key_mode mode) const noexcept { return limits::key_min(mode); }
  /// \brief Returns the maximal key size in bytes for specified keys mode.
  size_t key_max(key_mode mode) const { return limits::key_max(*this, mode); }
  /// \brief Returns the minimal value size in bytes for specified values mode.
  size_t value_min(value_mode mode) const noexcept { return limits::value_min(mode); }
  /// \brief Returns the maximal value size in bytes for specified values mode.
  size_t value_max(value_mode mode) const { return limits::value_max(*this, mode); }
  /// \brief Returns the maximal write transaction size (i.e. limit for summary
  /// volume of dirty pages) in bytes.
  size_t transaction_size_max() const { return limits::transaction_size_max(this->get_pagesize()); }

  /// \brief Make a copy (backup) of an existing environment to the specified
  /// path.
#ifdef MDBX_STD_FILESYSTEM_PATH
  env &copy(const MDBX_STD_FILESYSTEM_PATH &destination, bool compactify, bool force_dynamic_size = false);
#endif /* MDBX_STD_FILESYSTEM_PATH */
#if defined(_WIN32) || defined(_WIN64) || defined(DOXYGEN)
  env &copy(const ::std::wstring &destination, bool compactify, bool force_dynamic_size = false);
  env &copy(const wchar_t *destination, bool compactify, bool force_dynamic_size = false);
#endif /* Windows */
  env &copy(const ::std::string &destination, bool compactify, bool force_dynamic_size = false);
  env &copy(const char *destination, bool compactify, bool force_dynamic_size = false);

  /// \brief Copy an environment to the specified file descriptor.
  env &copy(filehandle fd, bool compactify, bool force_dynamic_size = false);

  /// \brief Deletion modes for \ref remove().
  enum remove_mode {
    /// \brief Just delete the environment's files and directory if any.
    /// \note On POSIX systems, processes already working with the database will
    /// continue to work without interference until it close the environment.
    /// \note On Windows, the behavior of `just_remove` is different
    /// because the system does not support deleting files that are currently
    /// memory mapped.
    just_remove = MDBX_ENV_JUST_DELETE,
    /// \brief Make sure that the environment is not being used by other
    /// processes, or return an error otherwise.
    ensure_unused = MDBX_ENV_ENSURE_UNUSED,
    /// \brief Wait until other processes closes the environment before deletion.
    wait_for_unused = MDBX_ENV_WAIT_FOR_UNUSED
  };

  /// \brief Removes the environment's files in a proper and multiprocess-safe way.
#ifdef MDBX_STD_FILESYSTEM_PATH
  static bool remove(const MDBX_STD_FILESYSTEM_PATH &pathname, const remove_mode mode = just_remove);
#endif /* MDBX_STD_FILESYSTEM_PATH */
#if defined(_WIN32) || defined(_WIN64) || defined(DOXYGEN)
  static bool remove(const ::std::wstring &pathname, const remove_mode mode = just_remove);
  static bool remove(const wchar_t *pathname, const remove_mode mode = just_remove);
#endif /* Windows */
  static bool remove(const ::std::string &pathname, const remove_mode mode = just_remove);
  static bool remove(const char *pathname, const remove_mode mode = just_remove);

  /// \brief Statistics for a database in the MDBX environment.
  using stat = ::MDBX_stat;

  /// \brief Information about the environment.
  using info = ::MDBX_envinfo;

  /// \brief Returns snapshot statistics about the MDBX environment.
  inline stat get_stat() const;

  /// \brief Returns pagesize of this MDBX environment.
  size_t get_pagesize() const { return get_stat().ms_psize; }

  /// \brief Return snapshot information about the MDBX environment.
  inline info get_info() const;

  /// \brief Return statistics about the MDBX environment accordingly to the specified transaction.
  inline stat get_stat(const txn &) const;

  /// \brief Return information about the MDBX environment accordingly to the specified transaction.
  inline info get_info(const txn &) const;

  /// \brief Returns the file descriptor for the DXB file of MDBX environment.
  inline filehandle get_filehandle() const;

  /// \brief Return the path that was used for opening the environment.
  path get_path() const;

  /// Returns environment flags.
  inline MDBX_env_flags_t get_flags() const;

  inline bool is_readonly() const { return (get_flags() & MDBX_RDONLY) != 0; }

  inline bool is_exclusive() const { return (get_flags() & MDBX_EXCLUSIVE) != 0; }

  inline bool is_cooperative() const { return !is_exclusive(); }

  inline bool is_writemap() const { return (get_flags() & MDBX_WRITEMAP) != 0; }

  inline bool is_readwite() const { return !is_readonly(); }

  inline bool is_nested_transactions_available() const { return (get_flags() & (MDBX_WRITEMAP | MDBX_RDONLY)) == 0; }

  /// \brief Returns the maximum number of threads/reader slots for the environment.
  /// \see extra_runtime_option::max_readers
  inline unsigned max_readers() const;

  /// \brief Returns the maximum number of named tables for the environment.
  /// \see extra_runtime_option::max_maps
  inline unsigned max_maps() const;

  /// \brief Returns the application context associated with the environment.
  inline void *get_context() const noexcept;

  /// \brief Sets the application context associated with the environment.
  inline env &set_context(void *your_context);

  /// \brief Sets threshold to force flush the data buffers to disk, for
  /// non-sync durability modes.
  ///
  /// \details The threshold value affects all processes which operates with
  /// given environment until the last process close environment or a new value
  /// will be settled. Data is always written to disk when \ref
  /// txn_managed::commit() is called, but the operating system may keep it
  /// buffered. MDBX always flushes the OS buffers upon commit as well, unless
  /// the environment was opened with \ref whole_fragile, \ref lazy_weak_tail or
  /// in part \ref half_synchronous_weak_last.
  ///
  /// The default is 0, than mean no any threshold checked, and no additional
  /// flush will be made.
  /// \see extra_runtime_option::sync_bytes
  inline env &set_sync_threshold(size_t bytes);

  /// \brief Gets threshold used to force flush the data buffers to disk, for
  /// non-sync durability modes.
  ///
  /// \copydetails set_sync_threshold()
  /// \see extra_runtime_option::sync_bytes
  inline size_t sync_threshold() const;

#if __cplusplus >= 201103L || defined(DOXYGEN)
  /// \brief Sets relative period since the last unsteady commit to force flush
  /// the data buffers to disk, for non-sync durability modes.
  ///
  /// \details The relative period value affects all processes which operates
  /// with given environment until the last process close environment or a new
  /// value will be settled. Data is always written to disk when \ref
  /// txn_managed::commit() is called, but the operating system may keep it
  /// buffered. MDBX always flushes the OS buffers upon commit as well, unless
  /// the environment was opened with \ref whole_fragile, \ref lazy_weak_tail or
  /// in part \ref half_synchronous_weak_last. Settled period don't checked
  /// asynchronously, but only by the \ref txn_managed::commit() and \ref
  /// env::sync_to_disk() functions. Therefore, in cases where transactions are
  /// committed infrequently and/or irregularly, polling by \ref
  /// env::poll_sync_to_disk() may be a reasonable solution to timeout
  /// enforcement.
  ///
  /// The default is 0, than mean no any timeout checked, and no additional
  /// flush will be made.
  /// \see extra_runtime_option::sync_period
  inline env &set_sync_period(const duration &period);

  /// \brief Gets relative period since the last unsteady commit that used to
  /// force flush the data buffers to disk, for non-sync durability modes.
  /// \copydetails set_sync_period(const duration&)
  /// \see set_sync_period(const duration&)
  /// \see extra_runtime_option::sync_period
  inline duration sync_period() const;
#endif

  /// \copydoc set_sync_period(const duration&)
  /// \param [in] seconds_16dot16  The period in 1/65536 of second when a
  /// synchronous flush would be made since the last unsteady commit.
  inline env &set_sync_period__seconds_16dot16(unsigned seconds_16dot16);

  /// \copydoc sync_period()
  /// \see sync_period__seconds_16dot16(unsigned)
  inline unsigned sync_period__seconds_16dot16() const;

  /// \copydoc set_sync_period(const duration&)
  /// \param [in] seconds  The period in second when a synchronous flush would
  /// be made since the last unsteady commit.
  inline env &set_sync_period__seconds_double(double seconds);

  /// \copydoc sync_period()
  /// \see set_sync_period__seconds_double(double)
  inline double sync_period__seconds_double() const;

  /// \copydoc MDBX_option_t
  enum class extra_runtime_option {
    /// \copydoc MDBX_opt_max_db
    /// \see max_maps() \see env::operate_parameters::max_maps
    max_maps = MDBX_opt_max_db,
    /// \copydoc MDBX_opt_max_readers
    /// \see max_readers() \see env::operate_parameters::max_readers
    max_readers = MDBX_opt_max_readers,
    /// \copydoc MDBX_opt_sync_bytes
    /// \see sync_threshold() \see set_sync_threshold()
    sync_bytes = MDBX_opt_sync_bytes,
    /// \copydoc MDBX_opt_sync_period
    /// \see sync_period() \see set_sync_period()
    sync_period = MDBX_opt_sync_period,
    /// \copydoc MDBX_opt_rp_augment_limit
    rp_augment_limit = MDBX_opt_rp_augment_limit,
    /// \copydoc MDBX_opt_loose_limit
    loose_limit = MDBX_opt_loose_limit,
    /// \copydoc MDBX_opt_dp_reserve_limit
    dp_reserve_limit = MDBX_opt_dp_reserve_limit,
    /// \copydoc MDBX_opt_txn_dp_limit
    dp_limit = MDBX_opt_txn_dp_limit,
    /// \copydoc MDBX_opt_txn_dp_initial
    dp_initial = MDBX_opt_txn_dp_initial,
    /// \copydoc MDBX_opt_spill_max_denominator
    spill_max_denominator = MDBX_opt_spill_max_denominator,
    /// \copydoc MDBX_opt_spill_min_denominator
    spill_min_denominator = MDBX_opt_spill_min_denominator,
    /// \copydoc MDBX_opt_spill_parent4child_denominator
    spill_parent4child_denominator = MDBX_opt_spill_parent4child_denominator,
    /// \copydoc MDBX_opt_merge_threshold_16dot16_percent
    merge_threshold_16dot16_percent = MDBX_opt_merge_threshold_16dot16_percent,
    /// \copydoc MDBX_opt_writethrough_threshold
    writethrough_threshold = MDBX_opt_writethrough_threshold,
    /// \copydoc MDBX_opt_prefault_write_enable
    prefault_write_enable = MDBX_opt_prefault_write_enable,
  };

  /// \copybrief mdbx_env_set_option()
  inline env &set_extra_option(extra_runtime_option option, uint64_t value);

  /// \copybrief mdbx_env_get_option()
  inline uint64_t extra_option(extra_runtime_option option) const;

  /// \brief Alter environment flags.
  inline env &alter_flags(MDBX_env_flags_t flags, bool on_off);

  /// \brief Set all size-related parameters of environment.
  inline env &set_geometry(const geometry &size);

  /// \brief Flush the environment data buffers.
  /// \return `True` if sync done or no data to sync, or `false` if the
  /// environment is busy by other thread or none of the thresholds are reached.
  inline bool sync_to_disk(bool force = true, bool nonblock = false);

  /// \brief Performs non-blocking polling of sync-to-disk thresholds.
  /// \return `True` if sync done or no data to sync, or `false` if the
  /// environment is busy by other thread or none of the thresholds are reached.
  bool poll_sync_to_disk() { return sync_to_disk(false, true); }

  /// \brief Close a key-value map (aka table) handle. Normally
  /// unnecessary.
  ///
  /// Closing a table handle is not necessary, but lets \ref txn::open_map()
  /// reuse the handle value. Usually it's better to set a bigger
  /// \ref env::operate_parameters::max_maps, unless that value would be
  /// large.
  ///
  /// \note Use with care.
  /// This call is synchronized via mutex with other calls \ref close_map(), but
  /// NOT with other transactions running by other threads. The "next" version
  /// of libmdbx (\ref MithrilDB) will solve this issue.
  ///
  /// Handles should only be closed if no other threads are going to reference
  /// the table handle or one of its cursors any further. Do not close a
  /// handle if an existing transaction has modified its table. Doing so can
  /// cause misbehavior from database corruption to errors like
  /// \ref MDBX_BAD_DBI (since the DB name is gone).
  inline void close_map(const map_handle &);

  /// \brief Reader information
  struct reader_info {
    int slot;                 ///< The reader lock table slot number.
    mdbx_pid_t pid;           ///< The reader process ID.
    mdbx_tid_t thread;        ///< The reader thread ID.
    uint64_t transaction_id;  ///< The ID of the transaction being read,
                              ///< i.e. the MVCC-snapshot number.
    uint64_t transaction_lag; ///< The lag from a recent MVCC-snapshot,
                              ///< i.e. the number of committed write
                              /// transactions since the current read
                              /// transaction started.
    size_t bytes_used;        ///< The number of last used page in the MVCC-snapshot
                              ///< which being read, i.e. database file can't be shrunk
                              ///< beyond this.
    size_t bytes_retained;    ///< The total size of the database pages that
                              ///< were retired by committed write transactions
                              ///< after the reader's MVCC-snapshot, i.e. the space
                              ///< which would be freed after the Reader releases
                              ///< the MVCC-snapshot for reuse by completion read
                              ///< transaction.

    MDBX_CXX11_CONSTEXPR reader_info(int slot, mdbx_pid_t pid, mdbx_tid_t thread, uint64_t txnid, uint64_t lag,
                                     size_t used, size_t retained) noexcept;
  };

  /// \brief Enumerate readers.
  ///
  /// The VISITOR class must have `int operator(const reader_info&, int serial)`
  /// which should return \ref continue_loop (zero) to continue enumeration,
  /// or any non-zero value to exit.
  ///
  /// \returns The last value returned from visitor' functor.
  template <typename VISITOR> inline int enumerate_readers(VISITOR &visitor);

  /// \brief Checks for stale readers in the lock table and
  /// return number of cleared slots.
  inline unsigned check_readers();

  /// \brief Sets a Handle-Slow-Readers callback to resolve database
  /// full/overflow issue due to a reader(s) which prevents the old data from
  /// being recycled.
  ///
  /// Such callback will be triggered in a case where there is not enough free
  /// space in the database due to long read transaction(s) which impedes
  /// reusing the pages of an old MVCC snapshot(s).
  ///
  /// Using this callback you can choose how to resolve the situation:
  ///  - abort the write transaction with an error;
  ///  - wait for the read transaction(s) to complete;
  ///  - notify a thread performing a long-lived read transaction
  ///    and wait for an effect;
  ///  - kill the thread or whole process that performs the long-lived read
  ///    transaction;
  ///
  /// \see long-lived-read
  inline env &set_HandleSlowReaders(MDBX_hsr_func *);

  /// \brief Returns the current Handle-Slow-Readers callback used to resolve
  /// database full/overflow issue due to a reader(s) which prevents the old
  /// data from being recycled.
  /// \see set_HandleSlowReaders()
  inline MDBX_hsr_func *get_HandleSlowReaders() const noexcept;

  /// \brief Starts read (read-only) transaction.
  inline txn_managed start_read() const;

  /// \brief Creates but not start read transaction.
  inline txn_managed prepare_read() const;

  /// \brief Starts write (read-write) transaction.
  inline txn_managed start_write(txn &parent);

  /// \brief Starts write (read-write) transaction.
  inline txn_managed start_write(bool dont_wait = false);

  /// \brief Tries to start write (read-write) transaction without blocking.
  inline txn_managed try_start_write();
};

/// \brief Managed database environment.
///
/// As other managed classes, `env_managed` destroys the represented underlying
/// object from the own class destructor, but disallows copying and assignment
/// for instances.
///
/// An environment supports multiple key-value tables (aka key-value spaces
/// or maps), all residing in the same shared-memory mapped file.
class LIBMDBX_API_TYPE env_managed : public env {
  using inherited = env;
  /// delegated constructor for RAII
  MDBX_CXX11_CONSTEXPR env_managed(MDBX_env *ptr) noexcept : inherited(ptr) {}
  void setup(unsigned max_maps, unsigned max_readers = 0);

public:
  MDBX_CXX11_CONSTEXPR env_managed() noexcept = default;

  /// \brief Open existing database.
#ifdef MDBX_STD_FILESYSTEM_PATH
  env_managed(const MDBX_STD_FILESYSTEM_PATH &pathname, const operate_parameters &, bool accede = true);
#endif /* MDBX_STD_FILESYSTEM_PATH */
#if defined(_WIN32) || defined(_WIN64) || defined(DOXYGEN)
  env_managed(const ::std::wstring &pathname, const operate_parameters &, bool accede = true);
  explicit env_managed(const wchar_t *pathname, const operate_parameters &, bool accede = true);
#endif /* Windows */
  env_managed(const ::std::string &pathname, const operate_parameters &, bool accede = true);
  explicit env_managed(const char *pathname, const operate_parameters &, bool accede = true);

  /// \brief Additional parameters for creating a new database.
  /// \see env_managed(const ::std::string &pathname, const create_parameters &,
  /// const operate_parameters &, bool accede)
  struct create_parameters {
    env::geometry geometry;
    mdbx_mode_t file_mode_bits{0640};
    bool use_subdirectory{false};
    MDBX_CXX11_CONSTEXPR create_parameters() noexcept = default;
    create_parameters(const create_parameters &) noexcept = default;
  };

  /// \brief Create new or open existing database.
#ifdef MDBX_STD_FILESYSTEM_PATH
  env_managed(const MDBX_STD_FILESYSTEM_PATH &pathname, const create_parameters &, const operate_parameters &,
              bool accede = true);
#endif /* MDBX_STD_FILESYSTEM_PATH */
#if defined(_WIN32) || defined(_WIN64) || defined(DOXYGEN)
  env_managed(const ::std::wstring &pathname, const create_parameters &, const operate_parameters &,
              bool accede = true);
  explicit env_managed(const wchar_t *pathname, const create_parameters &, const operate_parameters &,
                       bool accede = true);
#endif /* Windows */
  env_managed(const ::std::string &pathname, const create_parameters &, const operate_parameters &, bool accede = true);
  explicit env_managed(const char *pathname, const create_parameters &, const operate_parameters &, bool accede = true);

  /// \brief Explicitly closes the environment and release the memory map.
  ///
  /// Only a single thread may call this function. All transactions, tables,
  /// and cursors must already be closed before calling this function. Attempts
  /// to use any such handles after calling this function will cause a
  /// `SIGSEGV`. The environment handle will be freed and must not be used again
  /// after this call.
  ///
  /// \param [in] dont_sync  A dont'sync flag, if non-zero the last checkpoint
  /// will be kept "as is" and may be still "weak" in the \ref lazy_weak_tail
  /// or \ref whole_fragile modes. Such "weak" checkpoint will be ignored
  /// on opening next time, and transactions since the last non-weak checkpoint
  /// (meta-page update) will rolledback for consistency guarantee.
  void close(bool dont_sync = false);

  env_managed(env_managed &&) = default;
  env_managed &operator=(env_managed &&other) noexcept {
    if (MDBX_UNLIKELY(handle_))
      MDBX_CXX20_UNLIKELY {
        assert(handle_ != other.handle_);
        close();
      }
    inherited::operator=(std::move(other));
    return *this;
  }
  env_managed(const env_managed &) = delete;
  env_managed &operator=(const env_managed &) = delete;
  virtual ~env_managed() noexcept;
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
