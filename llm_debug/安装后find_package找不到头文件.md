# 安装后 find_package(qlgl) 找不到头文件（INTERFACE_INCLUDE_DIRECTORIES 丢失安装目录）

## 现象

库安装到任意 prefix 后，外部工程用 `find_package(qlgl)` + `target_link_libraries(app PRIVATE qlgl::qlgl)` 时，
编译直接失败：

```
main.c:1:10: fatal error: qlgl/qlgl.h: 没有那个文件或目录
```

`find_package` 本身成功（configure 不报错），错误只出现在**编译期**——头文件目录根本没进消费者的 include 路径。

## 复现步骤

```bash
# 1. 构建并安装到任意 prefix
cmake -S qlgl -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/tmp/prefix
cmake --build build -j
cmake --install build

# 2. 外部消费者工程
mkdir consumer && cd consumer
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(consumer C)
find_package(qlgl 0.1 REQUIRED)
add_executable(hello main.c)
target_link_libraries(hello PRIVATE qlgl::qlgl)
EOF
printf '#include <qlgl/qlgl.h>\nint main(void){return 0;}\n' > main.c
cmake -S . -B build -DCMAKE_PREFIX_PATH=/tmp/prefix
cmake --build build   # ← 这里失败
```

## 根因

`CMakeLists.txt` 第 20 行：

```cmake
target_include_directories(qlgl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
```

`$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>` 在**目标定义时**求值，
而 `include(GNUInstallDirs)` 在**第 518 行**才执行——晚于第 20 行。
此时 `CMAKE_INSTALL_INCLUDEDIR` 还是空字符串，于是 INSTALL_INTERFACE 展开成空，
导出的 `qlglTargets.cmake` 里只剩依赖库的 include 目录：

```cmake
set_target_properties(qlgl::qlgl PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "/usr/include;/usr/include/freetype2"
  ...
)
```

`${_IMPORT_PREFIX}/include` 这一项**整个丢失**。头文件确实装到了
`prefix/include/qlgl/`（`install(DIRECTORY ...)` 用的是第 518 行之后的
`CMAKE_INSTALL_INCLUDEDIR`，所以安装本身是对的），但导出文件不知道它在哪。

注意：`qlgl.pc` 没受此 bug 影响——它用 `${pcfiledir}` 相对定位，
`includedir=${prefix}/include` 是正确的。所以 pkg-config 路径的头文件没问题
（但 pkg-config 路径有另一个问题，见《pkg-config消费者缺能力宏.md》）。

## 验证

- 复现安装后 `grep INTERFACE_INCLUDE_DIRECTORIES prefix/lib/cmake/qlgl/qlglTargets.cmake`
  → 只有 `/usr/include;/usr/include/freetype2`，无 `${_IMPORT_PREFIX}/include`。
- 消费者手动加 `-DCMAKE_C_FLAGS="-I<prefix>/include"` 后编译链接全部通过
  → 证明唯一缺口就是这一条 include 目录。
- 把 `include(GNUInstallDirs)` 移到 `add_library(qlgl ...)` 之后、
  `target_include_directories` 之前，重新安装后导出文件变为
  `INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include;/usr/include;/usr/include/freetype2"`，
  消费者工程不加任何额外参数即可编译链接运行。

## 修复建议

把 `include(GNUInstallDirs)` 从第 518 行移到 `add_library(qlgl STATIC ...)` 之后
（第 20 行 `target_include_directories` 之前）。一行移动即可，已验证修复有效。

## 影响面

- 只影响**安装给第三方**的使用路径（README「在自己的工程里用」一节承诺的
  `find_package(qlgl 0.1 REQUIRED)` 流程）；仓库内部构建不受影响。
- 与 e7794b1（fix(install)）修的是同一类"只在消费方暴露"的问题，
  该提交修了 `PkgConfig::WAYLAND_EGL` 缺失和头文件循环，但漏了这条。
