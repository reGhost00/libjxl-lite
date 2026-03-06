# JPEG XL reference implementation

<img src="doc/jxl.svg" width="100" align="right" alt="JXL logo">

这是 [libjxl](https://github.com/libjxl/libjxl) 的精简版，用于给需要以子模块方式嵌入项目的更轻量级选择

1. 仅保留核心库（编码/解码）
2. 仅支持 x86 下的 msys2 / gcc / clang
3. 固定使用 skcms

### libjxl-lite 工具（当前项目）
1. cli: 命令行的编码/解码器
2. gui: 带图形界面的编码/解码器

你可以在此处找到文档：[document](https://libjxl.readthedocs.io/en/latest/)

如需更多资料，请参考 [libjxl](https://github.com/libjxl/libjxl)

## License

This software is available under a 3-clause BSD license which can be found in
the [LICENSE](LICENSE) file, with an "Additional IP Rights Grant" as outlined in
the [PATENTS](PATENTS) file.

Please note that the PATENTS file only mentions Google since Google is the legal
entity receiving the Contributor License Agreements (CLA) from all contributors
to the JPEG XL Project, including the initial main contributors to the JPEG XL
format: Cloudinary and Google.
