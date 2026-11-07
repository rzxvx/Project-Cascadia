# Frozen Cascadia dependencies

A12 owns the exact m1n1 base patches and Python/USB helpers it needs rather than
consuming the changing A13 track. `DEPENDENCIES.json` records source paths,
checkout commit and SHA-256 of every copied file. Apply `m1n1/base/0*.patch`
first, then A12's top-level m1n1 patches in the existing order.

Original copyright/license headers are retained. The sources originate in
Project Cascadia; individual helpers retain their MIT/GPL notices. This is not
Apple firmware. Dependencies include PyUSB/libusb for usbliter8ctl, standard
Python for the converters, and git/build tools for m1n1.

`mtfw.py` deliberately preserves the local A12-tested working-tree version,
including the pre-existing uncommitted edit in A13. Its manifest flags this;
other copies match their committed source. The original A13 file has since been
restored to its committed version; the A12-specific extension lives only here.
Freezing dependencies is not a clean-build or hardware-regression result.
