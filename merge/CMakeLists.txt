cmake_minimum_required(VERSION 3.15)
project(ffmpeg_merge VERSION 1.0.1)


# 作用范围是整个文件
include_directories(ffmpeg-n4.4.1-msvc/include)

# 指定库文件搜索路径
link_directories(ffmpeg-n4.4.1-msvc/bin)

SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/../bin)

# 用于创建可执行文件
add_executable(${PROJECT_NAME} "merge.cpp")

# 用于创建库文件
# add_library()

# 作用范围仅限于当前目标
target_include_directories(${PROJECT_NAME} PUBLIC 
    ffmpeg-n4.4.1-msvc/include
)

# 链接库文件到目标文件，public表示任何引用当前目标文件的目标，自动继承依赖
target_link_libraries(${PROJECT_NAME} PUBLIC
    avcodec
    avdevice
    avfilter
    avformat
    avutil
    postproc
    swresample
    swscale
)

