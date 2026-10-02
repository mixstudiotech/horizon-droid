# Run from the directory containing hzn_test and hzn_guest:
# timeout 90 gdb -q -batch -x gdb-smoke.gdb --args ./hzn_test ./hzn_guest --gdb-inferior
set pagination off
set confirm off
set follow-fork-mode child
set detach-on-fork on
set non-stop off
set auto-solib-add off
catch fork
catch exec
run
# The Horizon image is not ELF. Discard loader symbols before its exec.
symbol-file
continue
symbol-file ./hzn_guest
delete breakpoints
break *debug_worker_fn
continue
python
assert len(gdb.selected_inferior().threads()) == 2, "GDB did not track the Horizon child"
end
info threads
info registers x0 x1 sp pc v0 fpsr fpcr
x/4gx $sp
set $x2 = $x2
set $v0 = $v0
set $fpsr = $fpsr
set $fpcr = $fpcr
delete breakpoints
hbreak *(debug_worker_fn + 4)
continue
python
assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("(unsigned long)&debug_worker_fn")) + 4
end
delete breakpoints
set $old_pc = $pc
stepi
python
assert int(gdb.parse_and_eval("$pc")) != int(gdb.parse_and_eval("$old_pc")), "single step did not advance"
end
watch -l debug_worker_result
continue
python
assert len(gdb.selected_inferior().threads()) == 2, "all-stop lost a thread"
end
continue
continue
python
assert int(gdb.parse_and_eval("debug_worker_result")) == 1, "breaks did not resume"
end
delete breakpoints
continue
python
assert int(gdb.parse_and_eval("$_exitcode")) == 5, "unexpected Horizon exit code"
print("GDB_SMOKE_PASS")
end
quit
