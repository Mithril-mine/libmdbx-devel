/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT, VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#pragma once

#include "base.h++"

void osal_setup(const std::vector<actor_config> &actors);
void osal_broadcast(unsigned id);
int osal_waitfor(unsigned id);

int osal_actor_start(const actor_config &config, mdbx_pid_t &pid);
actor_status osal_actor_info(const mdbx_pid_t pid);
void osal_killall_actors(void);
int osal_actor_poll(mdbx_pid_t &pid, unsigned timeout);
void osal_wait4barrier(void);

bool osal_progress_push(bool active);
bool osal_multiactor_mode(void);

int osal_delay(unsigned seconds);
void osal_udelay(size_t us);
bool osal_istty(int fd);
std::string osal_tempdir(void);

#ifdef _MSC_VER
#ifndef STDIN_FILENO
#define STDIN_FILENO _fileno(stdin)
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO _fileno(stdout)
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO _fileno(stderr)
#endif
#endif /* _MSC_VER */

#if !defined(_WIN32) && !defined(_WIN64)
const char *signal_name(const int sig);
#endif /* Windows */
