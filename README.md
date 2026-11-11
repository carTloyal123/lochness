<img src="assets/icon.svg" width="64" height="64" alt="">

# lochness

Created in [T3 Code](https://t3.codes).

A privacy-first, local-only smart lock: a retrofit thumb-turn lock that is itself a Matter-over-Thread device, plus a wireless keypad. No hub.
No cloud, no accounts. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Host tests

The security-critical lock logic (`firmware/components/lock_core`) is plain C11 and runs on a laptop:

```sh
cmake -S firmware/test/host -B firmware/test/host/build -G Ninja
cmake --build firmware/test/host/build
ctest --test-dir firmware/test/host/build --output-on-failure
```

## License

Software is MIT (see [LICENSE](LICENSE)): use, copy, modify and distribute freely, keeping the copyright notice. The hardware designs in `hardware/` are CERN-OHL-S-2.0 (see [hardware/LICENSE](hardware/LICENSE)). Vendored third-party code keeps its own license: Monocypher (`firmware/components/monocypher`) is BSD-2-Clause or CC0, see its `LICENCE.md`.
