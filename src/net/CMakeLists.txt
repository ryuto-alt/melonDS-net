include(FixInterfaceIncludes)

add_library(net-utils STATIC
    Net.cpp
    Net_PCap.cpp
    Net_Slirp.cpp
    PacketDispatcher.cpp
    LocalMP.cpp
    LAN.cpp
    Netplay.cpp
    MPInterface.cpp
    NetplayProtocol.cpp
    NetplaySession.cpp
)

target_include_directories(net-utils PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")

# NetplaySession.cpp includes NDS.h and news up NDS instances, but this target
# does not link core, so it never picks up core's PUBLIC JIT_ENABLED. Compiling
# NDS.h both ways is an ODR violation: without JIT the ARMJIT member is gone and
# sizeof(NDS) drops by ~80MB, so `new NDS` under-allocates and the constructor
# (built with JIT) memsets a 64MB lookup table straight past the end of it.
if (ENABLE_JIT)
    target_compile_definitions(net-utils PUBLIC JIT_ENABLED)
endif()

option(USE_SYSTEM_LIBSLIRP "Use system libslirp instead of the bundled version" OFF)
if (USE_SYSTEM_LIBSLIRP)
    pkg_check_modules(Slirp REQUIRED IMPORTED_TARGET slirp)
    fix_interface_includes(PkgConfig::Slirp)
    target_link_libraries(net-utils PUBLIC PkgConfig::Slirp)
else()
    add_subdirectory(libslirp EXCLUDE_FROM_ALL)
    target_link_libraries(net-utils PUBLIC slirp)
endif()

if (USE_VCPKG)
    find_package(unofficial-enet CONFIG REQUIRED)
    target_link_libraries(net-utils PRIVATE unofficial::enet::enet)
else()
    pkg_check_modules(ENet REQUIRED IMPORTED_TARGET libenet)
    fix_interface_includes(PkgConfig::ENet)
    target_link_libraries(net-utils PUBLIC PkgConfig::ENet)
endif()

if (USE_VCPKG)
    find_package(miniupnpc CONFIG REQUIRED)
    target_link_libraries(net-utils PRIVATE miniupnpc::miniupnpc)
else()
    pkg_check_modules(MiniUPnPc REQUIRED IMPORTED_TARGET miniupnpc)
    fix_interface_includes(PkgConfig::MiniUPnPc)
    target_link_libraries(net-utils PUBLIC PkgConfig::MiniUPnPc)
endif()
