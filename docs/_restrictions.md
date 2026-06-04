Restrictions & Caveats {#restrictions}
======================
In addition to those listed for some functions.


## Long-lived read transactions {#long-lived-read}
Avoid long-lived read transactions, especially in the scenarios with a
high rate of write transactions. Long-lived read transactions prevents
recycling pages retired/freed by newer write transactions, thus the
database can grow quickly.

Understanding the problem of long-lived read transactions requires some
explanation, but can be difficult for quick perception. So is is
reasonable to simplify this as follows:
 * Garbage collection problem exists in all databases one way or
   another, e.g. VACUUM in PostgreSQL. But in _libmdbx_ it's even more
   discernible because of high transaction rate and intentional
   internals simplification in favor of performance.

 * _libmdbx_ employs [Multiversion concurrency control](https://en.wikipedia.org/wiki/Multiversion_concurrency_control)
   on the [Copy-on-Write](https://en.wikipedia.org/wiki/Copy-on-write)
   basis, that allows multiple readers runs in parallel with a write
   transaction without blocking. An each write transaction needs free
   pages to put the changed data, that pages will be placed in the new
   b-tree snapshot at commit. _libmdbx_ efficiently recycling pages from
   previous created unused _libmdbx_, BUT this is impossible if anyone
   a read transaction use such snapshot.

 * Thus massive altering of data during a parallel long read operation
   will increase the process's work set and may exhaust entire free
   database space.

A good example of long readers is a hot backup to the slow destination
or debugging of a client application while retaining an active read
transaction. LMDB this results in `MDB_MAP_FULL` error and subsequent write
performance degradation.

_libmdbx_ mostly solve "long-lived" readers issue by offering to use a
transaction parking-and-ousting approach by \ref mdbx_txn_park(),
Handle-Slow-Readers \ref MDBX_hsr_func callback which allows to abort
long-lived read transactions, and using the \ref MDBX_LIFORECLAIM mode
which addresses subsequent performance degradation. The "next" version
of libmdbx (aka \ref MithrilDB) will completely solve this.

Nonetheless, situations that encourage lengthy read transactions while
intensively updating data should be avoided. For example, you should
avoid suspending/blocking processes/threads performing read
transactions, including during debugging, and use transaction parking if
necessary.

You should also beware of aborting processes that perform reading
transactions. Despite the fact that libmdbx automatically checks and
cleans readers, as an a process aborting (especially with core dump) can
take a long time, and checking readers cannot be performed too often due
to performance degradation.

This issue will be addressed in _MithrilDB_ and one of libmdbx releases.
To do this, nonsequential GC recycling will be implemented, without stopping
garbage recycling on the old MVCC snapshot used by a long read transaction.

After the planned implementation, any long-term reading transaction will
still keep the used MVCC-snapshot (all the database pages forming it)
from being recycled, but it will allow all unused MVCC snapshots to be
recycled, both before and after the readable one. This will eliminate
one of the main architectural flaws inherited from LMDB and caused the
growth of a database in proportion to a volume of data changes made
concurrently with a long-running read transaction.


## Large data items

_libmdbx_ allows you to store values up to 1 gigabyte in size, but this is
not the main functionality for a key-value storage, but an additional
feature that should not be abused. Such long values are stored in
consecutive/adjacent DB pages, which has both pros and cons. This allows
you to read long values directly without copying and without any
overhead from a linear section of memory.

On the other hand, when putting such values in the database, it is
required to find a sufficient number of free consecutive/adjacent
database pages, which can be very difficult and expensive, moreover
sometimes impossible since b-tree tends to fragmentation. So, when
placing very long values, the engine may need to process the entire GC,
and in the absence of a sufficient sequence of free pages, increase the
DB file. Thus, for long values, _libmdbx_ provides maximum read performance
at the expense of write performance.

Some aspects related to GC have been refined and improved in 2022 within
the first releases of the 0.12.x series. In particular the search for
free consecutive/adjacent pages through GC has been significantly
speeded, including acceleration using NOEN/SSE2/AVX2/AVX512
instructions.

This issue will be addressed in _MithrilDB_ and refined within one of
0.15.x libmdbx releases.


### Huge transactions

A similar situation can be with huge transactions, in which a lot of
database pages are retired. The retired pages should be put into GC as a
list of page numbers for future reuse. But in huge transactions, such a
list of retired page numbers can also be huge, i.e. it is a very long
value and requires a long sequence of free pages to be saved. Thus, if
you delete large amounts of information from the database in a single
transaction, _libmdbx_ may need to increase the database file to save the
list of pages to be retired.

This issue was fixed in 2022 within the first releases of the 0.12.x
series by `Big Foot` feature, which now is enabled by default.
See \ref MDBX_ENABLE_BIGFOOT build-time option.

The `Big Foot` feature which significantly reduces GC overhead for
processing large lists of retired pages from huge transactions. Now
libmdbx avoid creating large chunks of PNLs (page number lists) which
required a long sequences of free pages, aka large/overflow pages. Thus
avoiding searching, allocating and storing such sequences inside GC.


## Space reservation
An _libmdbx_ database configuration will often reserve considerable unused
memory address space and maybe file size for future growth. This does
not use actual memory or disk space, but users may need to understand
the difference so they won't be scared off.

However, on 64-bit systems with a relative small amount of RAM, such
reservation can deplete system resources (trigger `ENOMEM` error, etc)
when setting an inadequately large upper DB size using \ref
mdbx_env_set_geometry() or \ref mdbx::env::geometry. So just avoid this.


## Remote filesystems
Do not use _libmdbx_ databases on remote filesystems, even between processes
on the same host. This breaks file locks on some platforms, possibly
memory map sync, and certainly sync between programs on different hosts.

On the other hand, _libmdbx_ support the exclusive database operation over
a network, and cooperative read-only access to the database placed on
a read-only network shares.


## Child processes
Do not use opened \ref MDBX_env instance(s) in a child processes after
`fork()`. It would be insane to call fork() and any MDBX-functions
simultaneously from multiple threads. The best way is to prevent the
presence of open MDBX-instances during `fork()`.

The \ref MDBX_ENV_CHECKPID build-time option, which is ON by default on
non-Windows platforms (i.e. where `fork()` is available), enables PID
checking at a few critical points. But this does not give any guarantees,
but only allows you to detect such errors a little sooner. Depending on
the platform, you should expect an application crash and/or database
corruption in such cases.

On the other hand, _libmdbx_ allow calling \ref mdbx_env_close() in such
cases to release resources, but no more and in general this is a wrong
way.

#### Since v0.13.1 and later
Starting from the v0.13.1 release, the \ref mdbx_env_resurrect_after_fork()
is available, which allows you to reuse an already open database
environment in child processes, but strictly without inheriting any
transactions from a parent process.


## Read-only mode
There is no pure read-only mode in a normal explicitly way, since
readers need write access to LCK-file to be ones visible for writer.

So _libmdbx_ always tries to open/create LCK-file for read-write, but switches
to without-LCK mode on appropriate errors (`EROFS`, `EACCESS`, `EPERM`)
if the read-only mode was requested by the \ref MDBX_RDONLY flag which is
described below.


## One thread - One transaction
A thread can only use one transaction at a time, plus any nested
read-write transactions in the non-writemap mode. Each transaction
belongs to one thread. The \ref MDBX_NOSTICKYTHREADS flag changes this,
see below.

Do not start more than one transaction for a one thread. If you think
about this, it's really strange to do something with two data snapshots
at once, which may be different. _libmdbx_ checks and preventing this by
returning corresponding error code (\ref MDBX_TXN_OVERLAPPING,
\ref MDBX_BAD_RSLOT, \ref MDBX_BUSY) unless you using \ref MDBX_NOSTICKYTHREADS
option on the environment. Nonetheless, with the `MDBX_NOSTICKYTHREADS`
option, you must know exactly what you are doing,
otherwise you will get deadlocks or reading an alien data.


#### Since v0.13.1 and later
Starting from the v0.13.1 release, the \ref mdbx_env_resurrect_after_fork()
is available, which allows you to reuse an already open database
environment in child processes, but strictly without inheriting any
transactions from a parent process.


## Do not open twice
Do not have open an _libmdbx_ database twice in the same process at the same
time. By default _libmdbx_ prevent this in most cases by tracking databases
opening and return \ref MDBX_BUSY if anyone LCK-file is already open.

The reason for this is that when the "Open file description" locks (aka
OFD-locks) are not available, _libmdbx_ uses POSIX locks on files, and these
locks have issues if one process opens a file multiple times. If a single
process opens the same environment multiple times, closing it once will
remove all the locks held on it, and the other instances will be
vulnerable to corruption from other processes.

For compatibility with LMDB which allows multi-opening, _libmdbx_ can be
configured at runtime by `mdbx_setup_debug(MDBX_DBG_LEGACY_MULTIOPEN, ...)`
prior to calling other _libmdbx_ functions. In this way _libmdbx_ will track
databases opening, detect multi-opening cases and then recover POSIX file
locks as necessary. However, lock recovery can cause unexpected pauses,
such as when another process opened the database in exclusive mode before
the lock was restored - we have to wait until such a process releases the
database, and so on.

## Troubleshooting the LCK-file

Within _libmdbx_, this file is always cleared when the first process starts
working with the database. Therefore, in any cases of damage to a LCK-file,
it is enough to close a database in all processes using it, or stop all
such processes before further restart.

### Corruption of LCK-file
A broken LCK-file can cause sync issues, including appearance of
wrong/inconsistent data for readers. When database opened in the
cooperative read-write mode the LCK-file requires to be mapped to memory
in read-write access. In this case it is always possible for
stray/malfunctioned application could writes thru pointers to silently
corrupt the LCK-file. There is no any portable way to prevent such
corruption, since the LCK-file is updated concurrently by multiple
processes in a lock-free manner and any locking is unwise due to a large
overhead.

### Stale reader(s)
Stale reader transactions left behind by an aborted program cause
further writes to grow the database quickly, and stale locks can block
further operation. _libmdbx_ checks for stale readers while opening
environment and before growth the database.

### Stale writer(s)
Stale writers will be cleared automatically by _libmdbx_. However this
is platform-specific, especially of implementation of shared
POSIX-mutexes and support for robust mutexes. For now there are no known
issues on all supported platforms.
