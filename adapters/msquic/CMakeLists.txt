# MoQ over raw QUIC on MsQuic.
#
# When the managed facade is enabled it shares this library rather than
# living in its own dylib: the doorbell facade (msquic_managed.c) calls an
# adapter-internal helper (moq_msq_conn_service_wake) that is inseparable
# from the hidden conn machinery in msquic_adapter.c, so under
# BUILD_SHARED_LIBS=ON the two translation units must resolve inside one
# shared object. moq::adapter-msquic-managed is therefore an INTERFACE over
# this base target (below). MOQ_BUILD_MSQUIC_MANAGED=OFF keeps the attach
# library lean and free of the threading dependency.
add_library(moq-adapter-msquic
    msquic_adapter.c
)
add_library(moq::adapter-msquic ALIAS moq-adapter-msquic)
if(MOQ_BUILD_MSQUIC_MANAGED)
    find_package(Threads REQUIRED)
    target_sources(moq-adapter-msquic PRIVATE msquic_managed.c)
endif()

target_include_directories(moq-adapter-msquic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# msquic::msquic is PUBLIC: the adapter header includes <msquic.h>.
target_link_libraries(moq-adapter-msquic
    PUBLIC moq::core msquic::msquic
)
if(MOQ_BUILD_MSQUIC_MANAGED)
    # The managed sources own the doorbell thread. PRIVATE: a shared base
    # dylib resolves pthread internally, and a static base propagates the
    # dependency to consumers that pull the managed objects.
    target_link_libraries(moq-adapter-msquic PRIVATE Threads::Threads)
endif()
target_compile_definitions(moq-adapter-msquic PRIVATE MOQ_BUILDING)
set_target_properties(moq-adapter-msquic PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(moq-adapter-msquic PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# Managed facade component. Its implementation lives in the base library
# above (shared-link ownership: the facade and the hidden helper it calls
# must share one dylib). This INTERFACE target is the stable public
# contract — moq::adapter-msquic-managed / find_package(... COMPONENTS
# adapter-msquic-managed) — carrying the base target and the threading
# dependency the managed objects need. No separate managed archive/dylib is
# produced; link the CMake target, not a library file.
if(MOQ_BUILD_MSQUIC_MANAGED)
    add_library(moq-adapter-msquic-managed INTERFACE)
    add_library(moq::adapter-msquic-managed ALIAS
        moq-adapter-msquic-managed)
    target_link_libraries(moq-adapter-msquic-managed INTERFACE
        moq::adapter-msquic Threads::Threads)
endif()

# -- Build-tree imported targets so `find_package(libmoq COMPONENTS
# adapter-msquic[-managed])` works against ${PROJECT_BINARY_DIR},
# mirroring the wtquic adapter. Install-tree counterparts below.
file(GENERATE
    OUTPUT "${PROJECT_BINARY_DIR}/libmoqMsquicAdapterTargets.cmake"
    CONTENT "\
if(NOT TARGET moq::adapter-msquic)\n\
  add_library(moq::adapter-msquic $<IF:$<STREQUAL:$<TARGET_PROPERTY:moq-adapter-msquic,TYPE>,STATIC_LIBRARY>,STATIC,SHARED> IMPORTED)\n\
  set_target_properties(moq::adapter-msquic PROPERTIES\n\
    IMPORTED_LOCATION \"$<TARGET_FILE:moq-adapter-msquic>\"\n\
    INTERFACE_INCLUDE_DIRECTORIES \"${CMAKE_CURRENT_SOURCE_DIR}/include\"\n\
    INTERFACE_LINK_LIBRARIES \"moq::core;msquic::msquic\")\n\
endif()\n")
if(MOQ_BUILD_MSQUIC_MANAGED)
    # INTERFACE imported target: the managed implementation ships inside the
    # attach library, so there is no managed artifact to point at.
    file(GENERATE
        OUTPUT "${PROJECT_BINARY_DIR}/libmoqMsquicManagedTargets.cmake"
        CONTENT "\
if(NOT TARGET moq::adapter-msquic-managed)\n\
  add_library(moq::adapter-msquic-managed INTERFACE IMPORTED)\n\
  set_target_properties(moq::adapter-msquic-managed PROPERTIES\n\
    INTERFACE_LINK_LIBRARIES \"moq::adapter-msquic;Threads::Threads\")\n\
endif()\n")
endif()

# -- Install / export --------------------------------------------------
include(GNUInstallDirs)

install(TARGETS moq-adapter-msquic
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/moq/msquic.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moq)
if(MOQ_BUILD_MSQUIC_MANAGED)
    # The managed facade is an INTERFACE over the attach library — no
    # artifact of its own. Only its header and the component target file
    # (below) are installed.
    install(FILES include/moq/msquic_managed.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moq)
endif()

# The installed component re-finds MsQuic through the same vendored
# module the build used.
install(FILES ${CMAKE_SOURCE_DIR}/cmake/FindMsQuic.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmoq)

# Relocatable install-tree imported target files (MsQuic is re-found by
# the component blocks in libmoqConfig).
set(_msquic_lib_name "$<TARGET_FILE_NAME:moq-adapter-msquic>")
set(_msquic_lib_type "$<IF:$<STREQUAL:$<TARGET_PROPERTY:moq-adapter-msquic,TYPE>,STATIC_LIBRARY>,STATIC,SHARED>")
set(_msquic_libdir "${CMAKE_INSTALL_LIBDIR}")
set(_msquic_incdir "${CMAKE_INSTALL_INCLUDEDIR}")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmoqMsquicAdapterTargets.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libmoqMsquicAdapterTargets-install.cmake.in
    @ONLY)
file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libmoqMsquicAdapterTargets-install.cmake
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/libmoqMsquicAdapterTargets-install.cmake.in)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libmoqMsquicAdapterTargets-install.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmoq
    RENAME libmoqMsquicAdapterTargets.cmake)
if(MOQ_BUILD_MSQUIC_MANAGED)
    # Relocatable managed component file: an INTERFACE imported target over
    # the installed attach library (no artifact of its own, so nothing to
    # locate). The template is placeholder-free; configure_file just stages
    # it into the install cmake dir.
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmoqMsquicManagedTargets.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/libmoqMsquicManagedTargets-install.cmake
        COPYONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libmoqMsquicManagedTargets-install.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmoq
        RENAME libmoqMsquicManagedTargets.cmake)
endif()

# No pkg-config file: MsQuic ships no .pc to chain a Requires: onto, so
# a .pc here would have to bake link paths. CMake package consumption
# is the supported path (see README).

if(MOQ_BUILD_TESTS)
    # Public-header compile/link smokes: <moq/msquic.h> stands alone,
    # in C and C++, against the build-tree target.
    add_executable(test_msquic_public_compile
        tests/test_msquic_public_compile.c)
    # the managed target carries the attach target publicly
    if(MOQ_BUILD_MSQUIC_MANAGED)
        target_link_libraries(test_msquic_public_compile PRIVATE
            moq::adapter-msquic-managed)
        target_compile_definitions(test_msquic_public_compile PRIVATE
            MOQ_HAVE_MSQUIC_MANAGED)
    else()
        target_link_libraries(test_msquic_public_compile PRIVATE
            moq::adapter-msquic)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(test_msquic_public_compile PRIVATE
            -Wall -Wextra -Wpedantic
            $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME msquic_public_compile COMMAND test_msquic_public_compile)

    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        enable_language(CXX)
        add_executable(test_msquic_public_compile_cxx
            tests/test_msquic_public_compile.cpp)
        if(MOQ_BUILD_MSQUIC_MANAGED)
            target_link_libraries(test_msquic_public_compile_cxx PRIVATE
                moq::adapter-msquic-managed)
            target_compile_definitions(test_msquic_public_compile_cxx
                PRIVATE MOQ_HAVE_MSQUIC_MANAGED)
        else()
            target_link_libraries(test_msquic_public_compile_cxx PRIVATE
                moq::adapter-msquic)
        endif()
        set_target_properties(test_msquic_public_compile_cxx PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        add_test(NAME msquic_public_compile_cxx
            COMMAND test_msquic_public_compile_cxx)
    endif()

    # White-box units over the fake QUIC_API_TABLE: compiles the
    # adapter source directly with the testing seams enabled — no real
    # MsQuic behavior involved.
    add_executable(test_msquic_unit
        tests/test_msquic_unit.c
        tests/support/fake_msq_table.c
        msquic_adapter.c)
    target_include_directories(test_msquic_unit PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/tests) # shared fake_endpoint for the rx pair
    target_compile_definitions(test_msquic_unit PRIVATE
        MOQ_MSQUIC_TESTING MOQ_BUILDING)
    target_link_libraries(test_msquic_unit PRIVATE
        moq::core msquic::msquic)
    target_include_directories(test_msquic_unit PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(test_msquic_unit PRIVATE
            -Wall -Wextra -Wpedantic
            $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME msquic_unit COMMAND test_msquic_unit)

    # Consumers: the same standalone project resolved against the
    # build tree (no install) and against a scratch install prefix.
    # Both request the managed component, which pulls the attach
    # component and re-finds MsQuic. Discovery hints from this build
    # are forwarded so the consumer resolves the same MsQuic.
    if(MOQ_BUILD_MSQUIC_MANAGED)
        add_test(NAME msquic_adapter_consumer
            COMMAND ${CMAKE_COMMAND}
                -DBUILD=${PROJECT_BINARY_DIR}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}/buildtree-consumer-work
                -DC_COMPILER=${CMAKE_C_COMPILER}
                "-DC_FLAGS=${CMAKE_C_FLAGS}"
                "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer_buildtree.cmake)
        set_tests_properties(msquic_adapter_consumer PROPERTIES
            TIMEOUT 180)

        add_test(NAME msquic_install_consumer
            COMMAND ${CMAKE_COMMAND}
                -DBUILD=${PROJECT_BINARY_DIR}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}/install-consumer-work
                "-DMSQUIC_DIR_HINT=${msquic_DIR}"
                "-DMSQUIC_ROOT_HINT=${MOQ_MSQUIC_ROOT}"
                -DC_COMPILER=${CMAKE_C_COMPILER}
                "-DC_FLAGS=${CMAKE_C_FLAGS}"
                "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer_install.cmake)
        set_tests_properties(msquic_install_consumer PROPERTIES
            TIMEOUT 300)

        # Shared-library link regression: a BUILD_SHARED_LIBS=ON tree must
        # build and link the managed component (the facade and its hidden
        # helper share one dylib). A static build cannot catch the
        # cross-dylib failure, so this configures a fresh shared tree.
        add_test(NAME msquic_managed_shared_link
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_SOURCE_DIR}
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}/managed-shared-link-work
                "-DMSQUIC_DIR_HINT=${msquic_DIR}"
                "-DMSQUIC_ROOT_HINT=${MOQ_MSQUIC_ROOT}"
                -DC_COMPILER=${CMAKE_C_COMPILER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/managed_shared_link.cmake)
        set_tests_properties(msquic_managed_shared_link PROPERTIES
            LABELS "msquic"
            TIMEOUT 600)
    endif()

    # Managed loopback: real MsQuic on localhost, self-signed certs,
    # managed client + managed server, one MoQ pub/sub flow.
    if(MOQ_BUILD_MSQUIC_MANAGED)
        # Discover openssl BEFORE the first registration guard that reads it.
        # find_program caches, so a discovery placed after its first use makes
        # the FIRST configure of a fresh build tree drop every test guarded
        # above it, while every later configure of the same tree looks correct.
        find_program(MOQ_OPENSSL openssl)

        # Inbound receive-backpressure recovery over real MsQuic loopback:
        # a stalled receiver backpressures the sender, and on release the
        # whole workload is delivered byte-exact and exactly once. Narrow
        # claims (see the test header); the exact arrest mechanics are in
        # the unit rail.
        add_executable(test_msquic_recv_loopback
            tests/test_msquic_recv_loopback.c)
        target_link_libraries(test_msquic_recv_loopback PRIVATE
            moq::adapter-msquic-managed Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_recv_loopback PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_recv_loopback
                COMMAND test_msquic_recv_loopback
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_recv_loopback PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 180)
        endif()

        add_executable(test_msquic_loopback tests/test_msquic_loopback.c)
        target_link_libraries(test_msquic_loopback PRIVATE
            moq::adapter-msquic-managed Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_loopback PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()

        if(MOQ_OPENSSL)
            add_test(NAME msquic_gen_certs
                COMMAND ${MOQ_OPENSSL} req -x509 -newkey rsa:2048
                    -sha256 -days 30 -nodes
                    -keyout ${CMAKE_CURRENT_BINARY_DIR}/key.pem
                    -out ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    -subj /CN=localhost)
            set_tests_properties(msquic_gen_certs PROPERTIES
                FIXTURES_SETUP msquic_certs
                LABELS "network;msquic")
            add_test(NAME msquic_loopback
                COMMAND test_msquic_loopback
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_loopback PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 120)
        endif()

        # Decisive lane-partition pin: server with two lanes, one lane
        # owning two connections; proves lane-scoped iteration, no
        # concurrent lane reentry, and terminal-before-reap visibility.
        add_executable(test_msquic_lanes tests/test_msquic_lanes.c)
        target_link_libraries(test_msquic_lanes PRIVATE
            moq::adapter-msquic-managed Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_lanes PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_lanes
                COMMAND test_msquic_lanes
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_lanes PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 120)
        endif()

        # Lane doorbell statistics: wake-arm classification, the
        # pending-cycle sample rule, idle-cap vs explicit causes, the
        # caller-context rule, struct_size prefix discipline, and a
        # real-transport flush observation under concurrent polling.
        add_executable(test_msquic_lane_stats
            tests/test_msquic_lane_stats.c)
        target_link_libraries(test_msquic_lane_stats PRIVATE
            moq::adapter-msquic-managed Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_lane_stats PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_lane_stats
                COMMAND test_msquic_lane_stats
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_lane_stats PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 120)
        endif()

        # Managed settings policy: the listener's pinned path MTU. Compiles
        # the adapter + managed sources directly (test seams on) so it asserts
        # the SAME settings construction the create path hands to
        # ConfigurationOpen(), not a reconstruction. Pure assertions -- no
        # socket, no certificates, no transport.
        add_executable(test_msquic_settings
            tests/test_msquic_settings.c
            msquic_adapter.c
            msquic_managed.c)
        target_include_directories(test_msquic_settings PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(test_msquic_settings PRIVATE
            MOQ_MSQUIC_TESTING MOQ_BUILDING)
        target_link_libraries(test_msquic_settings PRIVATE
            moq::core msquic::msquic Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_settings PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        add_test(NAME msquic_settings COMMAND test_msquic_settings)
        set_tests_properties(msquic_settings PROPERTIES
            LABELS "msquic" TIMEOUT 30)

        # Cross-lane doorbell wake delivery: the check->wait race, prompt
        # sub-idle-cap consumption, bidirectional wakes, coalescing, cause
        # classification, and wake-vs-stop. Needs the doorbell
        # pre-wait test hook, so it compiles the adapter + managed sources
        # directly with the testing seams on (test_msquic_unit-style) and
        # links no production adapter library.
        add_executable(test_msquic_wake
            tests/test_msquic_wake.c
            msquic_adapter.c
            msquic_managed.c)
        target_include_directories(test_msquic_wake PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(test_msquic_wake PRIVATE
            MOQ_MSQUIC_TESTING MOQ_BUILDING)
        target_link_libraries(test_msquic_wake PRIVATE
            moq::core msquic::msquic Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_wake PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_wake
                COMMAND test_msquic_wake
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_wake PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 120)
        endif()

        # Reaper scheduling pin: a child is not reclaimed while a pump is owed
        # (a pump OPPORTUNITY precedes further batch reclamation — not proof the
        # application consumed the terminal, which is separate later work). Uses
        # the reap-gap seam to make the unlock-window race deterministic, so it
        # compiles the adapter + managed sources with the testing seams on and
        # links no production adapter library.
        add_executable(test_msquic_reap
            tests/test_msquic_reap.c
            msquic_adapter.c
            msquic_managed.c)
        target_include_directories(test_msquic_reap PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(test_msquic_reap PRIVATE
            MOQ_MSQUIC_TESTING MOQ_BUILDING)
        target_link_libraries(test_msquic_reap PRIVATE
            moq::core msquic::msquic Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_reap PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_reap
                COMMAND test_msquic_reap
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_reap PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 120)
        endif()

        # Reap FAIRNESS pin, plus the pump-exit and stop controls for the new
        # reap site: an acknowledged, transport-reapable child must be
        # reclaimed while its lane still has work owed on every pass, not only
        # once the lane falls idle. NETWORK-FREE — the facade comes from a
        # testing constructor that opens no listener, socket or credential, so
        # this needs no cert fixture and carries no network label. Compiles the
        # adapter + managed sources with the testing seams on and links no
        # production adapter library.
        add_executable(test_msquic_reap_fairness
            tests/test_msquic_reap_fairness.c
            msquic_adapter.c
            msquic_managed.c)
        target_include_directories(test_msquic_reap_fairness PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(test_msquic_reap_fairness PRIVATE
            MOQ_MSQUIC_TESTING MOQ_BUILDING)
        target_link_libraries(test_msquic_reap_fairness PRIVATE
            moq::core msquic::msquic Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_reap_fairness PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        # No cert fixture and no network: the facade this test builds opens no
        # listener, so it is registered unconditionally.
        add_test(NAME msquic_reap_fairness COMMAND test_msquic_reap_fairness)
        set_tests_properties(msquic_reap_fairness PROPERTIES
            LABELS "msquic"
            TIMEOUT 120)

        # Terminal-child lifetime pin: a bounded application pump that leaves
        # SESSION_CLOSED unconsumed must still be given its child, its session
        # and its own per-connection state on a later pump. Kept out of
        # test_msquic_reap so that binary's narrower scope — a pump OPPORTUNITY
        # precedes further batch reclamation — stays separately reportable.
        # Uses the reap-gap and prewait seams to observe the doorbell's own
        # decision points, so it compiles the adapter + managed sources with the
        # testing seams on and links no production adapter library.
        add_executable(test_msquic_terminal_ack
            tests/test_msquic_terminal_ack.c
            msquic_adapter.c
            msquic_managed.c)
        target_include_directories(test_msquic_terminal_ack PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(test_msquic_terminal_ack PRIVATE
            MOQ_MSQUIC_TESTING MOQ_BUILDING)
        target_link_libraries(test_msquic_terminal_ack PRIVATE
            moq::core msquic::msquic Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_terminal_ack PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_terminal_ack
                COMMAND test_msquic_terminal_ack
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_terminal_ack PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 120)
        endif()

        # Production-contract conformance over real MsQuic loopback:
        # every MoQ session call runs inside the managed on_lane_pump,
        # driven by per-side event-triggered scripts (see
        # tests/msquic_contract_pair.h — deliberately not the shared
        # sim-shaped conformance rail).
        add_executable(test_msquic_conformance
            tests/test_msquic_conformance.c
            tests/msquic_contract_pair.c)
        target_link_libraries(test_msquic_conformance PRIVATE
            moq::adapter-msquic-managed Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_conformance PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_conformance
                COMMAND test_msquic_conformance
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_conformance PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 180)
        endif()

        # Stress/soak: repeated full-lifecycle churn over the same
        # script harness (no thresholds; MOQ_MSQUIC_STRESS_LOOPS
        # extends the run for manual soaks).
        add_executable(test_msquic_stress
            tests/test_msquic_stress.c
            tests/msquic_contract_pair.c)
        target_link_libraries(test_msquic_stress PRIVATE
            moq::adapter-msquic-managed Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_msquic_stress PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        if(MOQ_OPENSSL)
            add_test(NAME msquic_stress
                COMMAND test_msquic_stress
                    ${CMAKE_CURRENT_BINARY_DIR}/cert.pem
                    ${CMAKE_CURRENT_BINARY_DIR}/key.pem)
            set_tests_properties(msquic_stress PROPERTIES
                FIXTURES_REQUIRED msquic_certs
                LABELS "network;msquic"
                TIMEOUT 600)
        endif()

        # A configure-order guard that reads a find_program() result before the
        # find_program() runs is invisible from inside an already-configured
        # build tree, so this regression configures a throwaway CHILD build in
        # a directory it deletes first and compares its first- and
        # second-configure CTest name sets.
        #
        # MOQ_MSQUIC_REGISTRATION_CHILD is internal and test-only: the child
        # sets it so it does not register this fixture again. It defaults OFF,
        # so an ordinary configure still registers the top-level test, and the
        # fixture asserts the suppression actually took effect.
        # Validate the recorded branch HERE, in configure scope -- before and
        # independently of whether OpenSSL exists or this particular test is
        # registered. Doing it only inside the script would be unreachable for
        # an empty or unrecognized mode, because such a mode fails the
        # registration conjunction below and the script never runs: the test
        # would silently disappear instead of failing.
        if(NOT MOQ_MSQUIC_DISCOVERY_MODE STREQUAL "package"
           AND NOT MOQ_MSQUIC_DISCOVERY_MODE STREQUAL "root"
           AND NOT MOQ_MSQUIC_DISCOVERY_MODE STREQUAL "preexisting")
            message(FATAL_ERROR
                "MsQuic was found but FindMsQuic recorded discovery mode "
                "'${MOQ_MSQUIC_DISCOVERY_MODE}', which is not one of "
                "package/root/preexisting. Every branch that resolves MsQuic "
                "must record which one it was, or the configure-order "
                "regression silently stops being registered.")
        endif()

        # The recorded branch, surfaced so a nested control can observe it
        # without it having to live in (and pollute) the cache.
        message(STATUS
            "MsQuic discovery mode: ${MOQ_MSQUIC_DISCOVERY_MODE}")

        # Registered only where openssl was actually discovered: without it no
        # test is guarded on it in either pass, so the two passes agree
        # trivially and there is nothing for this fixture to protect. The child
        # is deliberately NOT told the parent's MOQ_OPENSSL -- passing it would
        # pre-seed the cache the child's own first configure must populate, and
        # that is exactly the state the defect hides in.
        #
        # Only for the two discovery branches an independent child can
        # reproduce. FindMsQuic also supports a caller-supplied
        # msquic::msquic target ("preexisting"); that dependency exists only
        # in the parent's own scope, so a nested configure cannot be given it
        # -- and inventing a fake package/root identity for it would test a
        # different dependency. In that mode this one test is omitted while
        # the rest of the MsQuic suite registers normally. Any other value
        # already failed the configure-scope check above -- it is not silently
        # dropped here.
        if(MOQ_OPENSSL AND NOT MOQ_MSQUIC_REGISTRATION_CHILD
           AND (MOQ_MSQUIC_DISCOVERY_MODE STREQUAL "package"
                OR MOQ_MSQUIC_DISCOVERY_MODE STREQUAL "root"))
            # Which of FindMsQuic's discovery branches ACTUALLY resolved
            # MsQuic this configure. Read from the module's own record, not
            # inferred from msquic_DIR: a nonempty msquic_DIR can be stale while
            # package discovery is disabled and the checkout fallback is what
            # really ran, and classifying that as package mode would send the
            # child off to whatever ambient package it can find.
            set(_reg_mode "${MOQ_MSQUIC_DISCOVERY_MODE}")
            # A search HINT only -- never the cached MOQ_OPENSSL result, which
            # would pre-seed the cache entry the child's own first pass must
            # populate. This keeps a parent pointed at a non-ambient openssl
            # from failing on a host where that directory is off CTest's PATH.
            # The executable itself is forwarded SEPARATELY, as the identity the
            # child's own discovery must land on.
            set(_reg_ssl_hint "")
            if(MOQ_OPENSSL)
                get_filename_component(_reg_ssl_hint "${MOQ_OPENSSL}" DIRECTORY)
            endif()
            add_test(NAME msquic_first_configure_registration
                COMMAND ${CMAKE_COMMAND}
                    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/first-configure-child
                    -DCMAKE_CMD=${CMAKE_COMMAND}
                    -DCTEST_CMD=${CMAKE_CTEST_COMMAND}
                    -DMSQUIC_MODE=${_reg_mode}
                    "-DMSQUIC_DIR=${msquic_DIR}"
                    "-DMSQUIC_ROOT=${MOQ_MSQUIC_ROOT}"
                    "-DOPENSSL_HINT_DIR=${_reg_ssl_hint}"
                    "-DEXPECT_OPENSSL=${MOQ_OPENSSL}"
                    "-DMSQUIC_INCLUDE_DIR=${MOQ_MSQUIC_INCLUDE_DIR}"
                    "-DMSQUIC_LIBRARY=${MOQ_MSQUIC_LIBRARY}"
                    -DEXPECT_RECV_LOOPBACK=$<BOOL:${MOQ_OPENSSL}>
                    -DSELF_TEST_NAME=msquic_first_configure_registration
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/first_configure_registration.cmake)
            set_tests_properties(msquic_first_configure_registration PROPERTIES
                LABELS "msquic"
                TIMEOUT 600)
        endif()

        # FindMsQuic's preexisting-target branch: prove a caller-supplied
        # msquic::msquic configures cleanly, records that branch, OMITS the
        # nested-configure regression above rather than registering it to fail,
        # and keeps the rest of the MsQuic suite. Registered only from the root
        # branch, which is where a concrete include dir and library to import
        # are known; the child it configures is a plain project configure, so it
        # needs no recursion guard of its own.
        if(NOT MOQ_MSQUIC_REGISTRATION_CHILD
           AND MOQ_MSQUIC_DISCOVERY_MODE STREQUAL "root"
           AND MOQ_MSQUIC_INCLUDE_DIR AND MOQ_MSQUIC_LIBRARY)
            add_test(NAME msquic_preexisting_target
                COMMAND ${CMAKE_COMMAND}
                    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/preexisting-child
                    -DCMAKE_CMD=${CMAKE_COMMAND}
                    -DCTEST_CMD=${CMAKE_CTEST_COMMAND}
                    "-DMSQUIC_INCLUDE_DIR=${MOQ_MSQUIC_INCLUDE_DIR}"
                    "-DMSQUIC_LIBRARY=${MOQ_MSQUIC_LIBRARY}"
                    "-DPARENT_OPENSSL=${MOQ_OPENSSL}"
                    -DSELF_TEST_NAME=msquic_first_configure_registration
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/preexisting_msquic_target.cmake)
            set_tests_properties(msquic_preexisting_target PROPERTIES
                LABELS "msquic"
                TIMEOUT 600)
        endif()
    endif()
endif()
