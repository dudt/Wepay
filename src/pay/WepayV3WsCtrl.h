// WePay V3 — 设备 WebSocket 控制器
// 设备连接后订阅自己的推送 topic，服务端下单时主动推送，无需设备轮询 pending
//
// 连接: ws://host/api/wepay/v3/ws
// 握手认证(连接后第一条消息):
//   { "action": "auth", "device_id": "xxx", "timestamp": "xxx",
//     "nonce": "xxx", "sign": "HMAC-SHA256 sorted-qs" }
// 认证成功后服务端发:
//   { "event": "authed", "device_id": "xxx" }
// 新订单推送 (服务端发给设备):
//   { "event": "new_order", "order_id": "xxx", "amount": "xx.xx",
//     "pay_type": "wxpay", "expired_at": 1234567890 }
// 心跳 keep-alive:
//   客户端: { "action": "ping" }
//   服务端: { "event": "pong", "server_time": 1234567890 }
#pragma once // 防止头文件重复包含
#include <drogon/WebSocketController.h> // Drogon WebSocket 控制器
#include <unordered_map> // 无序映射容器
#include <mutex> // 互斥锁
#include <json/json.h> // JSON 库
#include "../common/PayDb.h" // 数据库操作
#include "../common/Md5Utils.h" // MD5 和 HMAC-SHA256 工具
#include "../common/WsBus.h" // WebSocket 消息总线

// WePay V3 WebSocket 控制器类
class WepayV3WsCtrl : public drogon::WebSocketController<WepayV3WsCtrl> {
public:
    WS_PATH_LIST_BEGIN // WebSocket 路由列表开始
        WS_PATH_ADD("/api/wepay/v3/ws"); // 注册 WebSocket 路由
    WS_PATH_LIST_END // WebSocket 路由列表结束

    // 处理新消息（设备发送的消息）
    void handleNewMessage(const drogon::WebSocketConnectionPtr &conn,
                          std::string &&message,
                          const drogon::WebSocketMessageType &type) override {
        if (type == drogon::WebSocketMessageType::Ping) { // 如果是 Ping 消息
            conn->send("", drogon::WebSocketMessageType::Pong); return; // 回复 Pong
        }
        if (type != drogon::WebSocketMessageType::Text) return; // 只处理文本消息

        Json::Value j; Json::Reader reader; // 创建 JSON 读取器
        if (!reader.parse(message, j)) return; // 解析 JSON，失败则返回

        std::string action = j.get("action", "").asString(); // 获取 action 字段

        if (action == "auth") { // 如果是认证请求
            handleAuth(conn, j); return; // 处理认证
        }
        if (action == "ping") { // 如果是心跳 ping
            Json::Value r; r["event"] = "pong"; // 创建 pong 响应
            r["server_time"] = (Json::Int64)std::time(nullptr); // 添加服务器时间
            sendJson(conn, r); return; // 发送响应
        }
    }

    // 处理新连接（设备连接时调用）
    void handleNewConnection(const drogon::HttpRequestPtr &req,
                             const drogon::WebSocketConnectionPtr &conn) override {
        conn->setContext(std::make_shared<ConnCtx>()); // 为连接设置上下文
    }

    // 处理连接关闭（设备断开连接时调用）
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override {
        auto ctx = conn->getContext<ConnCtx>(); // 获取连接上下文
        if (ctx && !ctx->deviceId.empty()) { // 如果上下文存在且设备 ID 不为空
            WsBus::instance().unsubscribe("v3_dev_" + ctx->deviceId, conn); // 取消订阅设备 topic
            LOG_INFO << "[wepay_v3/ws] device disconnected: " << ctx->deviceId; // 记录日志
        }
        WsBus::instance().unsubscribeAll(conn); // 取消所有订阅
    }

private:
    // 连接上下文结构体（存储连接相关信息）
    struct ConnCtx {
        std::string deviceId; // 设备 ID
        bool authed = false; // 是否已认证
    };

    // 处理认证请求
    void handleAuth(const drogon::WebSocketConnectionPtr &conn, const Json::Value &j) {
        auto ctx = conn->getContext<ConnCtx>(); // 获取连接上下文
        if (!ctx) return; // 如果上下文不存在则返回
        if (ctx->authed) return; // 如果已认证则忽略重复认证

        std::string deviceId = j.get("device_id", "").asString(); // 获取设备 ID
        std::string timestamp = j.get("timestamp", "").asString(); // 获取时间戳
        std::string nonce = j.get("nonce", "").asString(); // 获取随机数
        std::string sign = j.get("sign", "").asString(); // 获取签名

        if (deviceId.empty() || timestamp.empty() || nonce.empty() || sign.empty()) { // 如果缺少认证参数
            sendError(conn, 400, "缺少认证参数"); conn->forceClose(); return; // 发送错误并关闭连接
        }

        // ═══════════════════════════════════════════════════════════════
        // 时间戳校验（防止重放攻击）
        // ═══════════════════════════════════════════════════════════════
        try {
            long long t = std::stoll(timestamp); // 将时间戳字符串转换为数字
            if (std::abs(std::time(nullptr) - t) > 60) { // 如果时间戳与当前时间相差超过 60 秒
                sendError(conn, 403, "时间戳过期"); conn->forceClose(); return; // 发送错误并关闭连接
            }
        } catch (...) { sendError(conn, 400, "时间戳格式错误"); conn->forceClose(); return; } // 时间戳格式错误

        // ═══════════════════════════════════════════════════════════════
        // Nonce 去重（防止重放攻击）
        // ═══════════════════════════════════════════════════════════════
        {
            std::lock_guard<std::mutex> lk(nonceMu_); // 加锁保护 nonce 集合
            if (usedNonces_.count(nonce)) { // 如果 nonce 已使用过
                sendError(conn, 403, "nonce 重复"); conn->forceClose(); return; // 发送错误并关闭连接
            }
            usedNonces_.insert(nonce); // 将 nonce 添加到已使用集合
            if (usedNonces_.size() > 100000) usedNonces_.clear(); // 如果集合过大则清空（防止内存溢出）
        }

        // ═══════════════════════════════════════════════════════════════
        // 签名校验
        // ═══════════════════════════════════════════════════════════════
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string key = db.getSetting("key"); // 获取默认密钥
        auto dev = db.queryOne("SELECT id,state,vmq_key FROM monitor_device WHERE device_id=?", {deviceId}); // 查询设备信息
        // 允许未注册设备连接（心跳会注册），但优先用设备对应商户 key
        auto bind = db.queryOne("SELECT mch_id FROM monitor_device_merchant WHERE device_id=? LIMIT 1", {deviceId}); // 查询设备绑定的商户
        if (!bind.empty()) { // 如果设备已绑定商户
            auto m = db.queryOne("SELECT vmq_key FROM merchant WHERE id=? AND state=1", {bind["mch_id"]}); // 查询商户密钥
            if (!m.empty() && !m["vmq_key"].empty()) key = m["vmq_key"]; // 使用商户密钥
        }

        std::string expected = v3Sign({{"device_id", deviceId}, {"nonce", nonce}, {"timestamp", timestamp}}, key); // 计算预期签名
        if (sign != expected) { // 如果签名不匹配
            sendError(conn, 403, "签名错误"); conn->forceClose(); return; // 发送错误并关闭连接
        }

        if (!dev.empty() && dev["state"] == "0") { // 如果设备已被禁用
            sendError(conn, 403, "设备已被禁用"); conn->forceClose(); return; // 发送错误并关闭连接
        }

        // ═══════════════════════════════════════════════════════════════
        // 认证通过，订阅设备专属 topic
        // ═══════════════════════════════════════════════════════════════
        ctx->deviceId = deviceId; // 保存设备 ID
        ctx->authed = true; // 标记为已认证
        WsBus::instance().subscribe("v3_dev_" + deviceId, conn); // 订阅设备专属 topic
        LOG_INFO << "[wepay_v3/ws] device authed: " << deviceId; // 记录日志

        Json::Value r; r["event"] = "authed"; r["device_id"] = deviceId; // 创建认证成功响应
        r["server_time"] = (Json::Int64)std::time(nullptr); // 添加服务器时间
        sendJson(conn, r); // 发送响应
    }

    // V3 签名计算（HMAC-SHA256 sorted-qs 格式）
    static std::string v3Sign(std::map<std::string, std::string> params, const std::string &secret) {
        std::string msg; // 签名消息
        for (auto &kv : params) { // 遍历参数
            if (!msg.empty()) msg += '&'; // 添加分隔符
            msg += kv.first + '=' + kv.second; // 添加参数
        }
        msg += "&key=" + secret; // 添加密钥
        return Md5Utils::hmacSha256(secret, msg); // 计算 HMAC-SHA256
    }

    // 发送 JSON 消息
    static void sendJson(const drogon::WebSocketConnectionPtr &conn, const Json::Value &j) {
        Json::StreamWriterBuilder wb; wb["indentation"] = ""; // 创建 JSON 写入器（无缩进）
        conn->send(Json::writeString(wb, j)); // 发送 JSON 字符串
    }

    // 发送错误消息
    static void sendError(const drogon::WebSocketConnectionPtr &conn, int code, const std::string &msg) {
        Json::Value r; r["event"] = "error"; r["code"] = code; r["msg"] = msg; // 创建错误响应
        sendJson(conn, r); // 发送错误响应
    }

    static std::mutex nonceMu_; // Nonce 集合的互斥锁
    static std::unordered_set<std::string> usedNonces_; // 已使用的 nonce 集合
};

inline std::mutex WepayV3WsCtrl::nonceMu_; // 初始化互斥锁
inline std::unordered_set<std::string> WepayV3WsCtrl::usedNonces_; // 初始化 nonce 集合
