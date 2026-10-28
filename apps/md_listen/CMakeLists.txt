# Market-data listener (Phase 3, issue #124). The feed handler lives in
# net/md_client and is part of lob_net; this target is only the command line.
add_executable(lob_md_listen main.cpp)
target_link_libraries(lob_md_listen PRIVATE lob lob_net)
