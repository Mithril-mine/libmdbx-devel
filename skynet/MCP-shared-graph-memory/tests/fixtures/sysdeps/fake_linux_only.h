/* Фейковый «системный заголовок» A: эмуляция mntent.h (Linux-only класс).
 * Используется тестами scan_system_deps через -I (не является системным по
 * пути — сканер подаёт его напрямую в per-header режим). */
#ifndef FAKE_MNTENT_H
#define FAKE_MNTENT_H

struct fake_mntent {
  const char *mnt_fsname;
  const char *mnt_dir;
};

void *fake_setmntent(const char *path, const char *mode);
struct fake_mntent *fake_getmntent(void *stream);
void fake_endmntent(void *stream);

#define FAKE_MNTTYPE_LXC "lxcfs"
#define FAKE_MOUNTED "/proc/mounts"

#endif /* FAKE_MNTENT_H */
