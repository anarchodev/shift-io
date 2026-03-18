add_executable(echo_server echo_server.c)
target_link_libraries(echo_server PRIVATE shift-io shift-io-sanitizers)
