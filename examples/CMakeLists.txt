find_package(Threads REQUIRED)

add_executable(echo_server echo_server.c)
target_link_libraries(echo_server PRIVATE shift-io shift-io-sanitizers Threads::Threads)

add_executable(load_test load_test.c)
target_link_libraries(load_test PRIVATE shift-io-sanitizers Threads::Threads)

add_executable(smoke_test smoke_test.c)
target_link_libraries(smoke_test PRIVATE shift-io shift-io-sanitizers)
