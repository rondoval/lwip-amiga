# TIME-WAIT reopen tests

Deterministic host tests (gcc + ASAN/UBSAN) of `port/amiga/tcp_tw_reopen.c`.
Real frames enter through `tcp_tw_reopen_input()` and go on to `ip_input()`, so
the filter and lwIP's `tcp_input()` both run; nothing in lwIP is patched.

```
./build.sh && ./twreopen_test && ./twreopen_test_nohook nohook
```

`twreopen_test_nohook` is the same reopen part built without the filter: it must
report failures, which shows the tests see the problem.
