# RTL8195B FDK AAC build

This directory vendors upstream `mstorsjo/fdk-aac` tag `v0.1.6`. This version
matches the public decoder and encoder version numbers found in the original
CarPlay `lib_fdkaac.a`.

Build the optimized static archive with:

```sh
make -f Makefile.rtl8195b
```

The output is `../../carplay_app/lib_fdkaac_o3.a`. The main firmware Makefile
invokes this build automatically when `CARBOX_FDK_AAC_OPTIMIZED=1` (the
default). Set it to `0` to link the original vendor archive instead.

The source remains subject to the Fraunhofer FDK AAC license in
`MODULE_LICENSE_FRAUNHOFER` and `NOTICE`.
