cmake_minimum_required(VERSION 3.15)
project(wepay-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 编译选项 ──────────────────────────────────────────────────
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /FS /utf-8")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -finput-charset=UTF-8 -fexec-charset=UTF-8")
    if(WIN32)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wa,-mbig-obj")
    endif()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
endif()

# ── 可选功能开关 ──────────────────────────────────────────────
option(ENABLE_REDIS    "启用 Redis 缓存后端(需要 hiredis)" OFF)
option(ENABLE_RABBITMQ "启用 RabbitMQ 消息队列(需要 rabbitmq-c)" OFF)

# ── 路径定义 ──────────────────────────────────────────────────
# Drogon 已用 CMake 安装到 need/install/ 前缀，直接复用其 include/lib
set(DROGON_INSTALL_PREFIX "G:/back/recovered/drogon-master/drogon-master/need/install-static"
    CACHE PATH "Path where drogon was installed (prefix containing include/ and lib/)")

# GoPay 库路径
set(GOPAY_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gopay-main/server")
set(GOPAY_INCLUDE_DIR "${GOPAY_LIB_DIR}")

set(DROGON_INCLUDE_DIRS
    "${DROGON_INSTALL_PREFIX}/include"
)

# ── 依赖查找 ──────────────────────────────────────────────────
if(WIN32)
    # 使用 MSYS2 ucrt64 工具链（与 Drogon 预编译 DLL 的 ABI 一致）
    set(MSYS_PREFIX "H:/msys64/msys64/ucrt64" CACHE PATH "MSYS2 ucrt64 root")
    set(VCPKG_INCLUDE_DIR "${MSYS_PREFIX}/include")
    set(VCPKG_LIB_DIR     "${MSYS_PREFIX}/lib")

    # PostgreSQL libpq 头文件
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libpq_include")
    foreach(_hdr libpq-fe.h libpq-events.h postgres_ext.h)
        if(EXISTS "${MSYS_PREFIX}/include/${_hdr}")
            file(COPY "${MSYS_PREFIX}/include/${_hdr}"
                 DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/libpq_include")
        endif()
    endforeach()
    set(POSTGRESQL_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/libpq_include")
    set(POSTGRESQL_LIB_DIR "${MSYS_PREFIX}/lib")

    find_library(DROGON_LIB   NAMES drogon  libdrogon  PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(TRANTOR_LIB  NAMES trantor libtrantor PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(JSONCPP_LIB  NAMES jsoncpp libjsoncpp PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(ZLIB_LIB     NAMES z zlib libz libzlib PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SSL_LIB      NAMES ssl     libssl     PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(CRYPTO_LIB   NAMES crypto  libcrypto  PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(HIREDIS_LIB  NAMES hiredis libhiredis PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(BROTLIENC_LIB    NAMES brotlienc    libbrotlienc    PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(BROTLIDEC_LIB    NAMES brotlidec    libbrotlidec    PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(BROTLICOMMON_LIB NAMES brotlicommon libbrotlicommon PATHS "${DROGON_INSTALL_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(PQ_LIB       NAMES pq libpq   PATHS ${POSTGRESQL_LIB_DIR} ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SQLITE3_LIB  NAMES sqlite3 libsqlite3 PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(MARIADB_LIB  NAMES mariadbclient libmariadbclient mariadb libmariadb PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(CURL_LIB     NAMES curl libcurl PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    unset(CARES_LIB CACHE)
    find_library(CARES_LIB    NAMES libcares.a cares libcares PATHS "${MSYS_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    message(STATUS "[c-ares] ${CARES_LIB}")
    find_library(QRENCODE_LIB NAMES qrencode libqrencode PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(ZXING_LIB    NAMES ZXing libZXing zxing libzxing PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SODIUM_LIB   NAMES sodium libsodium PATHS ${VCPKG_LIB_DIR} "${MSYS_PREFIX}/lib" NO_DEFAULT_PATH)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSONCPP  REQUIRED jsoncpp)
    pkg_check_modules(OPENSSL  REQUIRED openssl)
    find_library(DROGON_LIB  NAMES drogon  PATHS "${DROGON_INSTALL_PREFIX}/lib" /usr/local/lib /usr/lib)
    find_library(TRANTOR_LIB NAMES trantor PATHS "${DROGON_INSTALL_PREFIX}/lib" /usr/local/lib /usr/lib)
    find_library(PQ_LIB      NAMES pq)
    find_library(HIREDIS_LIB NAMES hiredis)
    find_library(SQLITE3_LIB NAMES sqlite3)
    find_library(ZLIB_LIB    NAMES z)
    find_library(CURL_LIB    NAMES curl)
    find_library(BROTLIENC_LIB    NAMES brotlienc)
    find_library(BROTLIDEC_LIB    NAMES brotlidec)
    find_library(BROTLICOMMON_LIB NAMES brotlicommon)
    find_library(QRENCODE_LIB NAMES qrencode)
    find_library(ZXING_LIB    NAMES ZXing zxing)
    find_library(SODIUM_LIB   NAMES sodium)
    set(VCPKG_INCLUDE_DIR "/usr/local/include")
    # Linux: 用 pkg-config 或常见路径找 libpq 头文件
    find_path(POSTGRESQL_INCLUDE_DIR libpq-fe.h
        PATHS /usr/include /usr/include/postgresql /usr/local/include
        PATH_SUFFIXES postgresql)
    if(NOT POSTGRESQL_INCLUDE_DIR)
        set(POSTGRESQL_INCLUDE_DIR "")
    endif()
endif()

if(NOT DROGON_LIB)
    message(FATAL_ERROR "libdrogon not found. Build Drogon first or set DROGON_INSTALL_PREFIX.")
endif()
if(NOT TRANTOR_LIB)
    message(FATAL_ERROR "libtrantor not found.")
endif()

message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Drogon:   ${DROGON_LIB}")
message(STATUS "Trantor:  ${TRANTOR_LIB}")
message(STATUS "libpq:    ${PQ_LIB}")

# ── 源文件 ──────────────────────────────────────────────────
file(GLOB_RECURSE SOURCES
    src/main.cc
    src/common/*.cc
    src/filters/*.cc
    src/pay/*.cc
    src/admin/*.cc
    src/gateway/*.cc
    src/notify/*.cc
    src/device/*.cc
    src/merchant/*.cc
    src/agent/*.cc
    src/channel/*.cc
)
add_executable(${PROJECT_NAME} ${SOURCES})

# ── 包含目录 ────────────────────────────────────────────────
set(JWT_CPP_INCLUDE "G:/back/recovered/drogon-master/void-engine/third_party/jwt-cpp/include"
    CACHE PATH "jwt-cpp header-only include directory")

if(WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DROGON_INCLUDE_DIRS}
        ${JWT_CPP_INCLUDE}
        ${VCPKG_INCLUDE_DIR}
        ${POSTGRESQL_INCLUDE_DIR}
        "${MSYS_PREFIX}/include"
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX DROGON_STATIC_DEFINE TRANTOR_STATIC_DEFINE JSONCPP_STATIC_DEFINE)
else()
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DROGON_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        /usr/local/include
        /usr/include
    )
endif()

# 启用 libpq 时的编译宏
if(PQ_LIB)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_LIBPQ=1)
endif()

# 启用 OpenSSL 时的编译宏（RSA 密钥生成）
if(SSL_LIB AND CRYPTO_LIB)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_OPENSSL=1)
endif()

# ── 可选: Redis(hiredis) ───────────────────────────────────
# 优先使用仓库内源码 G:/back/recovered/wepay-cpp/hiredis-master/hiredis-master
# 否则回退到 find_library
if(ENABLE_REDIS)
    set(HIREDIS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hiredis-master/hiredis-master"
        CACHE PATH "hiredis 源码目录")
    if(EXISTS "${HIREDIS_SRC_DIR}/CMakeLists.txt")
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(DISABLE_TESTS     ON  CACHE BOOL "" FORCE)
        set(ENABLE_SSL        OFF CACHE BOOL "" FORCE)
        add_subdirectory("${HIREDIS_SRC_DIR}" hiredis-build EXCLUDE_FROM_ALL)
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_REDIS=1)
        target_include_directories(${PROJECT_NAME} PRIVATE "${HIREDIS_SRC_DIR}/..")
        target_link_libraries(${PROJECT_NAME} PRIVATE hiredis)
        message(STATUS "Redis:    ENABLED (源码构建: ${HIREDIS_SRC_DIR})")
    else()
        if(WIN32)
            find_library(HIREDIS_LIB_EX NAMES hiredis libhiredis
                PATHS "${MSYS_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
        else()
            find_library(HIREDIS_LIB_EX NAMES hiredis)
        endif()
        if(HIREDIS_LIB_EX)
            target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_REDIS=1)
            target_link_libraries(${PROJECT_NAME} PRIVATE ${HIREDIS_LIB_EX})
            message(STATUS "Redis:    ENABLED -> ${HIREDIS_LIB_EX}")
        else()
            message(WARNING "ENABLE_REDIS=ON 但未找到 hiredis，已禁用")
        endif()
    endif()
endif()

# ── 可选: RabbitMQ(rabbitmq-c) ─────────────────────────────
# 优先使用仓库内源码 G:/back/recovered/wepay-cpp/rabbitmq-c-master/rabbitmq-c-master
if(ENABLE_RABBITMQ)
    set(RABBITMQ_C_SRC_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/rabbitmq-c-master/rabbitmq-c-master"
        CACHE PATH "rabbitmq-c 源码目录")
    if(EXISTS "${RABBITMQ_C_SRC_DIR}/CMakeLists.txt")
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
        set(BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
        set(BUILD_TOOLS_DOCS  OFF CACHE BOOL "" FORCE)
        set(BUILD_API_DOCS    OFF CACHE BOOL "" FORCE)
        set(ENABLE_SSL_SUPPORT OFF CACHE BOOL "" FORCE)
        add_subdirectory("${RABBITMQ_C_SRC_DIR}" rabbitmq-c-build EXCLUDE_FROM_ALL)
        if(WIN32 AND TARGET rabbitmq-static)
            # MinGW 下 rabbitmq-c 存在 int*/u_long* 类型不严格，降级为警告
            target_compile_options(rabbitmq-static PRIVATE
                -Wno-incompatible-pointer-types -Wno-error)
        endif()
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_RABBITMQ=1)
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${RABBITMQ_C_SRC_DIR}/include"
            "${CMAKE_CURRENT_BINARY_DIR}/rabbitmq-c-build/librabbitmq")
        target_link_libraries(${PROJECT_NAME} PRIVATE rabbitmq-static)
        message(STATUS "RabbitMQ: ENABLED (源码构建: ${RABBITMQ_C_SRC_DIR})")
    else()
        if(WIN32)
            find_library(RABBITMQ_LIB NAMES rabbitmq librabbitmq
                PATHS "${MSYS_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
        else()
            find_library(RABBITMQ_LIB NAMES rabbitmq)
        endif()
        if(RABBITMQ_LIB)
            target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_RABBITMQ=1)
            target_link_libraries(${PROJECT_NAME} PRIVATE ${RABBITMQ_LIB})
            message(STATUS "RabbitMQ: ENABLED -> ${RABBITMQ_LIB}")
        else()
            message(WARNING "ENABLE_RABBITMQ=ON 但未找到 librabbitmq，已禁用")
        endif()
    endif()
endif()

# ── 可选: WePay V3 原生监控插件 (redis++ / RocketMQ 可选) ────────
option(ENABLE_WEPAY_V3 "启用 WePay V3 原生监控插件（需要 redis++）" OFF)

if(ENABLE_WEPAY_V3)
    # 探测 redis++ (sw::redis::Redis)
    if(WIN32)
        find_library(REDISPP_LIB NAMES redis++ libredis++
            PATHS "${MSYS_PREFIX}/lib" ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    else()
        find_library(REDISPP_LIB NAMES redis++ libredis++)
    endif()

    if(REDISPP_LIB)
        file(GLOB V3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/channel/v3/*.cpp")
        # 未启用 RocketMQ 时排除 RocketMQManager.cpp（依赖 rocketmq SDK 头文件）
        if(NOT ENABLE_ROCKETMQ OR WIN32)
            list(FILTER V3_SOURCES EXCLUDE REGEX ".*RocketMQManager\\.cpp$")
        endif()
        # 探测 minio-cpp（miniocpp）
        find_package(miniocpp CONFIG QUIET)
        if(miniocpp_FOUND)
            target_link_libraries(${PROJECT_NAME} PRIVATE miniocpp::miniocpp)
            target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_MINIO=1)
            # INIReader (inih C++ wrapper, 被 miniocpp 内部使用)
            find_library(INIREADER_LIB NAMES INIReader)
            if(INIREADER_LIB)
                target_link_libraries(${PROJECT_NAME} PRIVATE ${INIREADER_LIB})
            endif()
            message(STATUS "minio-cpp: ENABLED")
        else()
            message(STATUS "minio-cpp: NOT FOUND, MinIO upload will be stubbed")
        endif()
        # Tesseract OCR + Leptonica（仅 Linux）
        find_library(TESSERACT_LIB NAMES tesseract)
        find_library(LEPTONICA_LIB NAMES lept)
        if(TESSERACT_LIB AND LEPTONICA_LIB)
            target_link_libraries(${PROJECT_NAME} PRIVATE ${TESSERACT_LIB} ${LEPTONICA_LIB})
            target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_TESSERACT=1)
            message(STATUS "Tesseract: ENABLED")
        else()
            message(STATUS "Tesseract: NOT FOUND, OCR will be stubbed")
        endif()
        # OcrService.cpp 始终编译，用宏控制实际实现
        target_sources(${PROJECT_NAME} PRIVATE ${V3_SOURCES})
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_V3=1)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${REDISPP_LIB})
        # MSYS2 把 redis++ 装到 ucrt64/include/sw/redis++/
        # 额外加 sw/ 子目录让 <redis++/redis++.h> 和 <sw/redis++/redis++.h> 都能工作
        if(WIN32)
            target_include_directories(${PROJECT_NAME} PRIVATE "${MSYS_PREFIX}/include/sw")
        else()
            target_include_directories(${PROJECT_NAME} PRIVATE "/usr/local/include/sw" "/usr/include/sw")
        endif()
        message(STATUS "WePay V3: ENABLED  (redis++: ${REDISPP_LIB})")

        # RocketMQ 仅 Linux 生产环境
        option(ENABLE_ROCKETMQ "启用 RocketMQ（仅 Linux 生产环境）" OFF)
        if(ENABLE_ROCKETMQ AND NOT WIN32)
            find_library(ROCKETMQ_LIB NAMES rocketmq)
            if(ROCKETMQ_LIB)
                target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_ROCKETMQ=1)
                target_link_libraries(${PROJECT_NAME} PRIVATE ${ROCKETMQ_LIB})
                message(STATUS "RocketMQ: ENABLED  -> ${ROCKETMQ_LIB}")
            else()
                message(WARNING "ENABLE_ROCKETMQ=ON 但未找到 librocketmq，已禁用")
            endif()
        elseif(ENABLE_ROCKETMQ AND WIN32)
            message(STATUS "RocketMQ: Windows 不支持，已跳过")
        endif()
    else()
        message(WARNING "ENABLE_WEPAY_V3=ON 但未找到 redis++，V3 插件已禁用")
    endif()
endif()

# ── 可选: Nacos 服务注册与配置中心 ────────────────────────────
option(ENABLE_NACOS "启用 Nacos 服务注册/配置中心(需要 nacos-sdk-cpp)" OFF)

if(ENABLE_NACOS)
    set(NACOS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/nacos/nacos-sdk-cpp-master"
        CACHE PATH "nacos-sdk-cpp 源码目录")
    if(UNIX AND EXISTS "${NACOS_SDK_DIR}/CMakeLists.txt")
        # Linux: 编译 nacos-sdk-cpp 为共享库 (.so)
        set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
        add_subdirectory("${NACOS_SDK_DIR}" nacos-sdk-build EXCLUDE_FROM_ALL)
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_NACOS=1 WEPAY_NACOS_SDK=1)
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${NACOS_SDK_DIR}"
            "${NACOS_SDK_DIR}/include")
        target_link_libraries(${PROJECT_NAME} PRIVATE nacos-cli)
        message(STATUS "Nacos:    ENABLED (Linux SDK .so)")
    elseif(CURL_LIB)
        # Windows / 无 SDK: 纯 HTTP API 实现，依赖已有 libcurl
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_NACOS=1)
        message(STATUS "Nacos:    ENABLED (HTTP API fallback, curl: ${CURL_LIB})")
    else()
        message(WARNING "ENABLE_NACOS=ON 但环境不满足，已禁用")
    endif()
endif()

# ── 自研 PG 驱动（drogon_pg_driver）─────────────────────────
option(ENABLE_PG_DRIVER "启用自研 PG 驱动层（连接池+熔断+异步写）" ON)

if(ENABLE_PG_DRIVER)
    set(PG_DRIVER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/drogon_pg_driver"
        CACHE PATH "自研 PG 驱动源码目录")
    if(EXISTS "${PG_DRIVER_SRC_DIR}/CMakeLists.txt")
        # 把已找到的 libpq 路径传给子项目，避免重复 find_package
        if(PQ_LIB)
            set(PostgreSQL_LIBRARIES    "${PQ_LIB}"                 CACHE STRING "" FORCE)
            set(PostgreSQL_INCLUDE_DIRS "${POSTGRESQL_INCLUDE_DIR}" CACHE STRING "" FORCE)
        endif()
        add_subdirectory("${PG_DRIVER_SRC_DIR}" pg_driver_build EXCLUDE_FROM_ALL)
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_PG_DRIVER=1)
        target_include_directories(${PROJECT_NAME} PRIVATE "${PG_DRIVER_SRC_DIR}/include")
        target_link_libraries(${PROJECT_NAME} PRIVATE drogon_pg_driver)
        message(STATUS "PgDriver: ENABLED  (${PG_DRIVER_SRC_DIR})")
    else()
        message(WARNING "ENABLE_PG_DRIVER=ON 但未找到 ${PG_DRIVER_SRC_DIR}，已禁用")
    endif()
endif()

# ── GoPay 集成 ──────────────────────────────────────────────
option(ENABLE_GOPAY "启用 GoPay 支付网关集成" ON)

if(ENABLE_GOPAY)
    if(WIN32)
        set(GOPAY_DLL "${GOPAY_LIB_DIR}/gopay.dll")
    else()
        set(GOPAY_DLL "${GOPAY_LIB_DIR}/gopay.so")
    endif()

    if(EXISTS "${GOPAY_DLL}")
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_GOPAY=1)
        target_include_directories(${PROJECT_NAME} PRIVATE "${GOPAY_INCLUDE_DIR}")
        target_link_libraries(${PROJECT_NAME} PRIVATE "${GOPAY_DLL}")
        message(STATUS "GoPay:    ENABLED -> ${GOPAY_DLL}")
    else()
        message(WARNING "ENABLE_GOPAY=ON 但未找到 ${GOPAY_DLL}，已禁用")
    endif()
endif()

# ── UPay shared 集成 ────────────────────────────────────────
option(ENABLE_UPAY_SHARED "启用 UPay shared 动态库桥接" ON)
set(UPAY_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pay/UPAY_PRO-main/shared"
    CACHE PATH "UPay shared 动态库目录")

if(ENABLE_UPAY_SHARED)
    if(WIN32)
        set(UPAY_SHARED_LIB "${UPAY_SHARED_DIR}/upay_shared.dll")
    else()
        set(UPAY_SHARED_LIB "${UPAY_SHARED_DIR}/libupay_shared.so")
    endif()

    if(EXISTS "${UPAY_SHARED_LIB}")
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_UPAY_SHARED=1)
        target_include_directories(${PROJECT_NAME} PRIVATE "${UPAY_SHARED_DIR}")
        message(STATUS "UPayShared: ENABLED -> ${UPAY_SHARED_LIB}")
    else()
        message(WARNING "ENABLE_UPAY_SHARED=ON 但未找到 ${UPAY_SHARED_LIB}，已禁用")
    endif()
endif()

# ── Alipay-Lib 集成 ──────────────────────────────────────────
option(ENABLE_ALIPAY "启用 Alipay-Lib 支付宝集成" ON)

if(ENABLE_ALIPAY)
    set(ALIPAY_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/alipay-lib")
    
    if(WIN32)
        set(ALIPAY_LIB "${ALIPAY_LIB_DIR}/libwepay-v3-windows.a")
    else()
        set(ALIPAY_LIB "${ALIPAY_LIB_DIR}/libwepay-v3-linux.a")
    endif()
    
    if(EXISTS "${ALIPAY_LIB}")
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_HAS_ALIPAY=1)
        target_include_directories(${PROJECT_NAME} PRIVATE "${ALIPAY_LIB_DIR}")
        target_link_libraries(${PROJECT_NAME} PRIVATE "${ALIPAY_LIB}")
        message(STATUS "Alipay:   ENABLED -> ${ALIPAY_LIB}")
    else()
        message(WARNING "ENABLE_ALIPAY=ON 但未找到 ${ALIPAY_LIB}，已禁用")
    endif()
endif()

# ── 链接库 ──────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${DROGON_LIB}
        ${TRANTOR_LIB}
        ${PQ_LIB}
        ${MARIADB_LIB}
        ${HIREDIS_LIB}
        ${SQLITE3_LIB}
        ${JSONCPP_LIB}
        ${SSL_LIB}
        ${CRYPTO_LIB}
        ${CURL_LIB}
        ${BROTLIENC_LIB}
        ${BROTLIDEC_LIB}
        ${BROTLICOMMON_LIB}
        ${ZLIB_LIB}
        ${QRENCODE_LIB}
        ${ZXING_LIB}
        ${SODIUM_LIB}
        ${CARES_LIB}
        ws2_32 wsock32 crypt32 secur32 bcrypt
        iphlpapi userenv rpcrt4 shlwapi winpthread psapi dbghelp
    )

    # ── xpay-go: 编译 + 复制子进程 ────────────────────────────────
    # 新版 mq/xpay-go: 模板/静态资源已嵌入 EXE，收款码存 DB，无需外部文件
    set(XPAY_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mq/xpay-go")
    set(XPAY_CFG_SRC "${XPAY_SRC_DIR}/config.yaml")
    set(XPAY_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/xpay-go")

    find_program(GO_EXECUTABLE go)
    if(GO_EXECUTABLE)
        message(STATUS "xpay-go: 找到 Go 编译器 ${GO_EXECUTABLE}，将自动编译 mq/xpay-go")
        # 独立 target：go build → 直接输出到 build/xpay-go/
        add_custom_target(xpay_go_build ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${XPAY_OUT_DIR}"
            COMMAND ${GO_EXECUTABLE} build
                    -o "${XPAY_OUT_DIR}/xpay.exe"
                    .
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XPAY_CFG_SRC}"
                    "${XPAY_OUT_DIR}/config.yaml"
            WORKING_DIRECTORY "${XPAY_SRC_DIR}"
            COMMENT "编译 mq/xpay-go → xpay.exe (内嵌模板, 收款码存DB)"
        )
        # 让 C++ 主目标在 go build 完成后才做 POST_BUILD
        add_dependencies(${PROJECT_NAME} xpay_go_build)
    else()
        message(WARNING "xpay-go: 未找到 go 编译器，尝试复制预编译的 xpay.exe")
        set(XPAY_EXE_SRC "${XPAY_SRC_DIR}/xpay.exe")
        if(EXISTS "${XPAY_EXE_SRC}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${XPAY_OUT_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XPAY_EXE_SRC}" "${XPAY_OUT_DIR}/xpay.exe"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XPAY_CFG_SRC}" "${XPAY_OUT_DIR}/config.yaml"
                COMMENT "复制预编译 mq/xpay-go/xpay.exe")
        else()
            message(WARNING "xpay-go: mq/xpay-go/xpay.exe 不存在，/xpay/** 将不可用")
        endif()
    endif()

    # ── 自动复制运行时 DLL 到 build 目录 ──
    set(_drogon_bin "${DROGON_INSTALL_PREFIX}/bin")
    set(_drogon_dlls
        libdrogon.dll libtrantor.dll libjsoncpp.dll
        libbrotlicommon.dll libbrotlidec.dll libbrotlienc.dll libz.dll
    )
    get_filename_component(_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_cxx_dlls libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    set(_msys_bin "${MSYS_PREFIX}/bin")
    set(_msys_dlls
        libpq.dll libhiredis.dll libsqlite3-0.dll
        libssl-3-x64.dll libcrypto-3-x64.dll
        libiconv-2.dll libintl-8.dll
        libcurl-4.dll libnghttp2-14.dll libnghttp3-9.dll
        libssh2-1.dll libpsl-5.dll
        libngtcp2-16.dll libngtcp2_crypto_ossl-0.dll
        libgsasl-18.dll libntlm-0.dll
        libgmp-10.dll libgnutls-30.dll libhogweed-6.dll libnettle-8.dll
        libtasn1-6.dll libp11-kit-0.dll libffi-8.dll libidn2-0.dll libunistring-5.dll
        libqrencode.dll libpng16-16.dll
        libZXing.dll
    )
    foreach(_dll IN LISTS _drogon_dlls)
        if(EXISTS "${_drogon_bin}/${_dll}")
            list(APPEND _runtime_dlls "${_drogon_bin}/${_dll}")
        endif()
    endforeach()
    foreach(_dll IN LISTS _cxx_dlls)
        if(EXISTS "${_cxx_bin}/${_dll}")
            list(APPEND _runtime_dlls "${_cxx_bin}/${_dll}")
        endif()
    endforeach()
    foreach(_dll IN LISTS _msys_dlls)
        if(EXISTS "${_msys_bin}/${_dll}")
            list(APPEND _runtime_dlls "${_msys_bin}/${_dll}")
        endif()
    endforeach()
    if(ENABLE_UPAY_SHARED AND EXISTS "${UPAY_SHARED_LIB}")
        list(APPEND _runtime_dlls "${UPAY_SHARED_LIB}")
    endif()
    # libpq 间接依赖 + libsodium
    file(GLOB _msys_extra
        "${_msys_bin}/libgssapi*.dll"
        "${_msys_bin}/libkrb5*.dll"
        "${_msys_bin}/libk5crypto*.dll"
        "${_msys_bin}/libcom_err*.dll"
        "${_msys_bin}/libsasl2*.dll"
        "${_msys_bin}/libidn2*.dll"
        "${_msys_bin}/libunistring*.dll"
        "${_msys_bin}/libzstd*.dll"
        "${_msys_bin}/libsodium*.dll"
    )
    list(APPEND _runtime_dlls ${_msys_extra})
    list(REMOVE_DUPLICATES _runtime_dlls)

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_runtime_dlls}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND_EXPAND_LISTS VERBATIM
        COMMENT "复制运行时 DLL 到 $<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${DROGON_LIB}
        ${TRANTOR_LIB}
        ${PQ_LIB}
        ${HIREDIS_LIB}
        ${SQLITE3_LIB}
        ${JSONCPP_LIBRARIES}
        ssl crypto
        ${ZLIB_LIB}
        curl uuid cares
        pthread dl
    )
    foreach(_optlib QRENCODE_LIB ZXING_LIB SODIUM_LIB)
        if(${_optlib})
            target_link_libraries(${PROJECT_NAME} PRIVATE ${${_optlib}})
        endif()
    endforeach()
    foreach(_brotli BROTLIENC_LIB BROTLIDEC_LIB BROTLICOMMON_LIB)
        if(${_brotli})
            target_link_libraries(${PROJECT_NAME} PRIVATE ${${_brotli}})
        endif()
    endforeach()
endif()

# ── 许可证管理工具 ────────────────────────────────────────────
add_executable(license_tool tools/license_tool.cc)
if(WIN32)
    target_include_directories(license_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common ${VCPKG_INCLUDE_DIR})
    target_link_directories(license_tool PRIVATE ${VCPKG_LIB_DIR})
    target_link_libraries(license_tool PRIVATE ssl crypto iphlpapi ws2_32)
else()
    target_include_directories(license_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(license_tool PRIVATE ssl crypto pthread)
endif()

# ── 嵌入前端（可选）─────────────────────────────────────────────
# 用法: cmake -DEMBED_FRONTEND=ON -DEMBED_FRONTEND_DIR=UI/dist ..
# 前端文件会编译进 exe，发布时只需一个 exe 即可
option(EMBED_FRONTEND "Embed frontend dist files into executable" OFF)
set(EMBED_FRONTEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/UI/dist" CACHE PATH "Frontend dist directory to embed")

if(EMBED_FRONTEND)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake)
    embed_web_resources("${EMBED_FRONTEND_DIR}" "${CMAKE_BINARY_DIR}/EmbeddedResources.generated.cc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/EmbeddedResources.generated.cc")
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEPAY_EMBEDDED_FRONTEND)
    message(STATUS "[Build] 前端嵌入模式: ON (from ${EMBED_FRONTEND_DIR})")
else()
    message(STATUS "[Build] 前端嵌入模式: OFF (使用外部 web/ 目录)")
endif()

# ── 复制配置文件（仅首次，不覆盖用户修改）─────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.json" AND NOT EXISTS "${CMAKE_BINARY_DIR}/config.json")
    configure_file(config.json ${CMAKE_BINARY_DIR}/config.json COPYONLY)
endif()
