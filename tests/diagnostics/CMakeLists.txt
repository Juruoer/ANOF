# 人工按需运行，不把环境时钟观测作为常规 CTest 断言。
add_executable(anof_timer_probe EXCLUDE_FROM_ALL TimerProbe.cc)
target_link_libraries(anof_timer_probe PRIVATE muduox_net)
target_compile_features(anof_timer_probe PRIVATE cxx_std_17)
set_target_properties(anof_timer_probe PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(anof_timer_probe PRIVATE -Wall -Wextra -Wconversion -Wshadow)
