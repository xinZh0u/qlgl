# 可视化 benchmark：两个可执行文件，同一个场景，只换后端。
#
#   cmake --build build --target bench_cpu bench_gl
#   ./build/benchmark/bench_cpu --size 1280x720
#   ./build/benchmark/bench_gl  --vsync 0 --frames 300
#
# 完全不要它们：cmake -S . -B build -DQLGL_BUILD_BENCHMARK=OFF

add_executable(bench_cpu bench_cpu.c)
target_link_libraries(bench_cpu PRIVATE qlgl)

# GL 后端没编进来时 bench_gl 仍会构建，只是打印一行说明并返回 1
add_executable(bench_gl bench_gl.c)
target_link_libraries(bench_gl PRIVATE qlgl)
