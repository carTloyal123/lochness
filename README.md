<img src="assets/icon.svg" width="64" height="64" alt="">

# lochness

Created in [T3 Code](https://t3.codes).

A privacy-first, local-only smart lock: Matter hub + retrofit thumb-turn lock + wireless keypad.
No cloud, no accounts. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Host tests

The security-critical lock logic (`firmware/components/lock_core`) is plain C11 and runs on a laptop:

```sh
cmake -S firmware/test/host -B firmware/test/host/build -G Ninja
cmake --build firmware/test/host/build
ctest --test-dir firmware/test/host/build --output-on-failure
```
