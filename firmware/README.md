# Firmware artifacts, kept per module

One codebase can now build for either module (`idf.py menuconfig` ->
`Modem module`), so a `.bin`/`.patch` built while one module was selected
is only ever valid for a device running that same module -- mixing them
in one flat folder was an easy way to diff or flash the wrong pair.
Hence a directory per module:

```
firmware/
  simcom_a7672_usb/
    full/    <- full .bin releases for CONFIG_MODEM_MODULE_SIMCOM_A7672_USB builds
    delta/   <- .patch files (tools/make_patch.py) between two of those .bin's
  quectel_ec200u_uart/
    full/    <- full .bin releases for CONFIG_MODEM_MODULE_QUECTEL_EC200U_UART builds
    delta/   <- .patch files between two of those .bin's
```

`.bin`/`.patch` files themselves are gitignored (see `.gitignore`) --
only the directory structure is tracked. Building a delta patch always
means: same module on both sides, `--base` matching what's currently
flashed on the target device (see `tools/make_patch.py --help`).
