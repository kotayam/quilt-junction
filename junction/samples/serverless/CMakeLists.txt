message(STATUS "Building junction sample: serverless")

# Generate test configurations
set(caladan_config_common
"host_netmask 255.255.255.0
host_gateway 192.168.127.1
runtime_kthreads 1
runtime_spinning_kthreads 0
runtime_guaranteed_kthreads 1
runtime_priority lc
runtime_quantum_us 0
enable_directpath 1
host_mtu 9000"
)

set(caladan_function_config
"host_addr 192.168.127.7
${caladan_config_common}"
)

set(caladan_function_config_path
    ${CMAKE_CURRENT_BINARY_DIR}/caladan_function.config
)
file(WRITE ${caldan_function_config_path} ${caladan_function_config})

# function
add_executable(function 
    function.cc
)
