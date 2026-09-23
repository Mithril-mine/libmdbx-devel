#!/usr/bin/env bash
#
# Copyright (c) 2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
#
# THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
# UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
# VIA ANY MEDIUM IS STRICTLY PROHIBITED.
#
# The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
# for any purpose other than the purpose for which they were provided to you.
#
# The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
# the warranties of merchantability, fitness for a particular purpose and non infringement.
# In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
# whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
# or the use or other dealings in the software.
#
# The above copyright notice and this permission notice shall be included in all copies
# or substantial portions of the software.

TMUX=tmux
DIR="$(dirname ${BASH_SOURCE[0]})"
TEST="${DIR}/stochastic.sh --skip-make --db-upto-gb 32"
PREFIX="/dev/shm/mdbxtest-"

NUMACTL="$(which numactl 2>&- || echo false)"
NUMALIST=()
NUMAIDX=0
if [ -n "${NUMACTL}" -a $(${NUMACTL} --hardware | grep 'node [0-9]\+ cpus' | wc -l) -gt 1 ]; then
	NUMALIST=($(${NUMACTL} --hardware | grep 'node [0-9]\+ cpus' | cut -d ' ' -f 2))
fi

function test_numacycle {
	NUMAIDX=$((NUMAIDX + 1))
	if [ ${NUMAIDX} -ge ${#NUMALIST[@]} ]; then
		NUMAIDX=0
	fi
}

function test_numanode {
	if [[ ${#NUMALIST[@]} > 1 ]]; then
		echo "${TEST} --numa ${NUMALIST[$NUMAIDX]}"
	else
		echo "${TEST}"
	fi
}

${TMUX} kill-session -t mdbx
rm -rf ${PREFIX}*
# git clean -x -f -d && make test-assertions
${TMUX} -f "${DIR}/tmux.conf" new-session -d -s mdbx htop

W=0
for ps in min 4k max; do
	for from in 1 30000; do
		for n in 0 1 2 3; do
			CMD="$(test_numanode) --delay $((n * 7)) --page-size ${ps} --from ${from} --dir ${PREFIX}page-${ps}.from-${from}.${n}"
			if [ $n -eq 0 ]; then
				${TMUX} new-window -t mdbx:$((++W)) -n "page-${ps}.from-${from}" -k -d "$CMD"
				${TMUX} select-layout -E tiled
			else
				${TMUX} split-window -t mdbx:$W -l 20% -d $CMD
			fi
			test_numacycle
		done
		for n in 0 1 2 3; do
			CMD="$(test_numanode) --delay $((3 + n * 7)) --extra --page-size ${ps} --from ${from} --dir ${PREFIX}page-${ps}.from-${from}.${n}-extra"
			if [ $n -eq 0 ]; then
				${TMUX} new-window -t mdbx:$((++W)) -n "page-${ps}.from-${from}-extra" -k -d "$CMD"
				${TMUX} select-layout -E tiled
			else
				${TMUX} split-window -t mdbx:$W -l 20% -d $CMD
			fi
			test_numacycle
		done
	done
done

${TMUX} attach -t mdbx
