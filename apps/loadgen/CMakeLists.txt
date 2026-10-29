add_executable(lob_loadgen main.cpp hdr_latency.cpp)
target_link_libraries(lob_loadgen PRIVATE lob lob_net
                                             hdr_histogram::hdr_histogram_static)
