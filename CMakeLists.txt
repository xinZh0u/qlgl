cmake_minimum_required(VERSION 3.10)

# 项目名称 & 语言
project(qlgl C)

# C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 头文件搜索路径
include_directories(${PROJECT_SOURCE_DIR}/include)

# 源文件
file(GLOB SRC_FILES
    src/*.c
    include/*.h
    doc/*.txt
)

# 生成可执行文件
add_executable(${PROJECT_NAME} ${SRC_FILES})