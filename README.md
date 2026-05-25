# volume-commander

A standalone, minimal GUI for **3D ink labelling** of Vesuvius Challenge scroll
volumes. Open a zarr/c3d volume (local or S3) and a flattened segment, view it
through four synchronized panels (XY / XZ / YZ axis slices + a composite-rendered
flattened surface), and paint a full-volume **binary 3D mask** to label ink — read
back on the composite flattened view.

Ported and rewritten from the adaptive software-render pipeline that lived in
volume-cartographer (VC3D) before it was removed. No segmentation, no ML, no
tracer, no OpenCV.

## Build

```
cmake -B build -G Ninja
cmake --build build
```

Requires a C++26 compiler, Qt6, CURL, blosc2, zlib, and the
[c3d](https://github.com/SuperOptimizer) codec (expected at `~/c3d`, override
with `-DC3D_DIR=...`).

## License

MIT.
