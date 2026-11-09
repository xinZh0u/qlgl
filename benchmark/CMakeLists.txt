# 可视化 benchmark：一个程序、两个后端、多个维度。
#
#   cmake --build build --target bench
#   ./build/benchmark/bench --list
#   ./build/benchmark/bench --backend cpu --scene cards
#
# 完全不要它：cmake -S . -B build -DQLGL_BUILD_BENCHMARK=OFF

add_executable(bench bench.c)
target_link_libraries(bench PRIVATE qlgl)
