set debuginfod enabled on
handle all pass nostop noprint
catch signal SIGABRT SIGBUS SIGSEGV
set follow-fork-mode child
set detach-on-fork off
handle SIGABRT SIGBUS SIGSEGV stop print
python gdb.events.exited.connect(lambda x : gdb.execute("quit"))
run
set pagination off
thread apply all bt
