# Order-entry gateway (decision C54). The executable is a thin shell over
# net/gateway; the root add_subdirectory hook picks this directory up.
add_executable(lob_gateway main.cpp)
target_link_libraries(lob_gateway PRIVATE lob lob_net)
