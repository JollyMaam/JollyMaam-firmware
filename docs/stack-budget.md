# What the "stack size limit exceeded" warnings mean

Every link prints a block like this:

    ld: warning: LLVM gold plugin: stack size limit exceeded (544) in hci_send_event_cmd_complete
    ld: warning: LLVM gold plugin: stack size limit exceeded (528) in execute_command
    ld: warning: LLVM gold plugin: stack size limit exceeded (260) in EccPoint_mult

## It is a lint, not a limit

The number comes from `--plugin-opt=-warn-stack-size=256` in the SDK's own
`SDK_LFLAGS` (`sdk-br23.mk`).  It is the vendor's lint: the linker plugin
estimates each function's frame and complains about anything above 256 bytes.
Nothing enforces it, and the vendor firmware trips it too for the same SDK
functions (`hci_send_event`, `EccPoint_mult`, `hmacCompute`, ...).  The compile
step carries the same idea as `-Wframe-larger-than=256`, but the SDK build
passes `-w`, so only the linker half is visible.

## The number that actually matters is the task stack

`src/main.c` sizes the tasks, and the sizes are **counts of 32-bit words, not
bytes** - `os_task_create()` documents its `stksize` as "unit (u32)".  So the
real budgets are four times the numbers in the table:

| task | table | bytes | runs |
|---|---|---|---|
| app_core | 640 | 2560 | the app loop, the USB console, every `sys_timer` callback (the timer task posts the callback to the task that registered it) |
| btstack | 2048 | 8192 | the BTstack host: HCI events, L2CAP, HID, SDP |
| btctrler | 512 | 2048 | the Jieli controller library |
| btencry | 512 | 2048 | BT crypto |
| sys_event | 256 | 1024 | system events |
| imu | 256 | 1024 | IMU sampling |
| systimer | 128 | 512 | timer bookkeeping only - it posts, it does not call |
| update | 512 | 2048 | the update path |

and the interrupt stack is `.irq_stack`, `0x848` = 2120 bytes.

For reference, the vendor's own HID app (`apps/hid/app_main.c`) uses the same
numbers except `btstack`, which it sizes at 768 words; this project raised that
to 2048 because the upstream host has deeper frames than the vendor host.

## Measuring it instead of guessing

`tools/stack_report.py` re-links with a low warning threshold to get a frame
size for (almost) every function, prints every linked object and SDK archive
back to LLVM IR with the vendor's clang, builds the call graph, and reports the
heaviest chain per root.  It stops at functions that hand work to another task
(`user_send_cmd_prepare`, `sys_timer_add`, the `os_taskq_*` posts), because
those callees run on a different stack.

With exact frames (threshold 0, i.e. nothing charged a default):

| root | chain | stack it runs on |
|---|---|---|
| `cyclone2_poll` | 1144 | app_core, 2560 |
| `bredr_rx_irq_handler` | 1132 | interrupt, 2120 |
| `event_handler` | 848 | btstack, 8192 |
| `hci_send_event` | 704 | btstack, 8192 |

The heaviest app_core chain is the console: `execute_command` (528) ->
`print_input_status` (240) -> `snprintf`.  The deepest interrupt chain goes
through the controller library's link-RX dispatch.

## Conclusion

Nothing in the image is near its stack limit: the two tightest are about 45%
and 53% used, and the BTstack task is around 10%.  The warnings are not
evidence of stack corruption and do not explain the resets - for that, see
`docs/reboot-hunting.md`.

The one thing worth keeping an eye on is `execute_command`: at 528 bytes it is
over half of app_core's 2560 on its own, and everything the console does runs
underneath it.  Adding another large console command with a big frame on top
of `print_*` would be the way to actually get into trouble.
