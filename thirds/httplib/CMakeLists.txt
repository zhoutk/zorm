# cpp-httplib (yhirose/cpp-httplib v0.52.0, MIT license)
# Header-only HTTP/HTTPS server & client library, used by maple/TileService.

add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Restore high thread count. Previous reduction to 12 was a misdiagnosis —
# the real bottleneck was keep-alive timeout holding HTTP threads idle while
# waiting for clients to send next request. With keep-alive disabled in
# TileService::start (set_keep_alive_max_count(1)), each request frees its
# thread immediately upon completion, so we can match higher concurrency.
# 64 threads handle ~64 concurrent connections; worker pool throttles
# downstream so HTTP threads naturally back-pressure via fut.get().
target_compile_definitions(httplib INTERFACE
    CPPHTTPLIB_THREAD_POOL_COUNT=64
    CPPHTTPLIB_LISTEN_BACKLOG=512
    # Shorten keep_alive() pre-check select() poll from 10ms to 1ms.
    # v0.52.0 calls keep_alive() before every connection's first request
    # even with keep-alive disabled; this reduces the per-connection
    # select() wait from 10ms→1ms, recovering ~0.3ms/request.
    CPPHTTPLIB_KEEPALIVE_TIMEOUT_CHECK_INTERVAL_USECOND=1000
)

if(WIN32)
    target_link_libraries(httplib INTERFACE ws2_32)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(httplib INTERFACE Threads::Threads)
endif()
