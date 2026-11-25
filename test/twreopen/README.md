# TIME-WAIT reopen tests

Deterministic host tests (gcc + ASAN/UBSAN) of `LWIP_TCP_TW_REOPEN`, the lwIP
fork's reopen of a TIME-WAIT connection on a new SYN (`lwip/src/core/tcp_in.c`).
Real packets enter through `ip_input()`, so lwIP's own checks and `tcp_input()`
all run.

```
./build.sh && ./twreopen_test && ./twreopen_test_off off
```

`twreopen_test_off` is the same reopen part built with the option off: it must
report failures, which shows the tests see the problem.
