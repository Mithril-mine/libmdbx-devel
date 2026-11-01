/* TU-фикстура для USES_SYS: использует символы обоих фейковых заголовков
 * плюс один не-объявленный (unresolved-класс, ловится сканером как
 * unresolved, а компилятором — как warning; тестируется через walk AST,
 * где implicit declaration всё равно порождает DeclRefExpr). */
#include "fake_linux_only.h"
#include "fake_common.h"

int tu_uses_sys(void) {
  void *s = fake_setmntent(FAKE_MOUNTED, "r");
  fake_endmntent(s);
  return fake_common_probe() + (int)FAKE_COMMON_VERSION;
}

/* unresolved-класс: намеренно без декларации */
int tu_uses_unresolved(void);
int tu_uses_unresolved(void) {
  extern int totally_unknown_api(int);
  return totally_unknown_api(42);
}
