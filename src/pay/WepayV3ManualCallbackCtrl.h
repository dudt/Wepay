// WePay V3 手动回调控制器
// GET /api/wepay/v3/callback/manual?order_id=xx&mch_id=xx&token=xx
//   → 验签 → 幂等检查 → NotifyTaskService::retryNow() → 返回 HTML 结果页
// POST /admin/api/wepay/v3/email/resend
//   → 管理端手动重发邮件（支付失败通知）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <json/json.h> // JSON 库
#include "../common/PayDb.h" // 数据库操作
#include "../common/WepayV3EmailService.h" // 邮件服务
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

// WePay V3 手动回调控制器类
class WepayV3ManualCallbackCtrl
    : public drogon::HttpController<WepayV3ManualCallbackCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(WepayV3ManualCallbackCtrl::manual,
                      "/api/wepay/v3/callback/manual",       drogon::Get); // 手动回调路由
        ADD_METHOD_TO(WepayV3ManualCallbackCtrl::emailResend,
                      "/admin/api/wepay/v3/email/resend",    drogon::Post,  "AdminAuthFilter"); // 重发邮件路由（需管理员认证）
        ADD_METHOD_TO(WepayV3ManualCallbackCtrl::emailLogs,
                      "/admin/api/wepay/v3/email/logs",      drogon::Get,   "AdminAuthFilter"); // 邮件日志路由（需管理员认证）
        ADD_METHOD_TO(WepayV3ManualCallbackCtrl::callbackLogs,
                      "/admin/api/wepay/v3/callback/logs",   drogon::Get,   "AdminAuthFilter"); // 回调日志路由（需管理员认证）
    METHOD_LIST_END // 路由列表结束

    // ══════════════════════════════════════════════════════════════
    // GET /api/wepay/v3/callback/manual — 手动回调（商户点击邮件链接）
    // ══════════════════════════════════════════════════════════════
    // 商户点击邮件里的按钮，浏览器直接访问该链接
    void manual(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string orderId = req->getParameter("order_id"); // 订单号
        std::string mchId   = req->getParameter("mch_id"); // 商户 ID
        std::string token   = req->getParameter("token"); // 令牌
        std::string ip      = req->getPeerAddr().toIp(); // 客户端 IP
        std::string ua      = req->getHeader("User-Agent"); // 用户代理

        // 1) 基本参数校验
        if (orderId.empty() || mchId.empty() || token.empty()) { // 检查必要参数
            htmlResp(cb, false, "", "缺少必要参数"); return; // 参数不完整
        }

        // 2) 令牌验证
        auto res = V3CallbackToken::verify(token); // 验证令牌
        if (!res.ok || res.orderId != orderId || res.mchId != mchId) { // 令牌无效或不匹配
            logCallback(orderId, mchId, token, false, "令牌验证失败", ip, ua); // 记录失败
            htmlResp(cb, false, orderId, "令牌无效或已过期"); return; // 返回错误
        }

        auto &db = PayDb::instance(); // 获取数据库实例

        // 3) 令牌一次性消费检查（先查数据库）
        auto rec = db.queryOne( // 查询回调记录
            "SELECT callback_status FROM v3_manual_callback_log WHERE order_id=? AND mch_id=?",
            {orderId, mchId});
        if (!rec.empty() && rec["callback_status"] == "1") { // 如果已回调
            htmlResp(cb, true, orderId, "回调已发送（重复点击）"); return; // 返回成功（幂等）
        }

        // 4) 查询订单
        auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId}); // 查询订单
        if (order.empty()) { // 订单不存在
            logCallback(orderId, mchId, token, false, "订单不存在", ip, ua); // 记录失败
            htmlResp(cb, false, orderId, "订单不存在"); return; // 返回错误
        }

        // 5) 触发回调（复用 NotifyTaskService 已有重试逻辑）
        NotifyTaskService::retryNow(orderId); // 立即重试通知
        long long now = std::time(nullptr); // 获取当前时间

        // 6) 更新回调记录
        db.exec( // 更新回调状态
            "UPDATE v3_manual_callback_log "
            "SET callback_status=1,callback_time=?,client_ip=?,user_agent=? "
            "WHERE order_id=? AND mch_id=?",
            {std::to_string(now), ip, ua.substr(0, 200), orderId, mchId});

        logCallback(orderId, mchId, token, true, "", ip, ua); // 记录成功
        htmlResp(cb, true, orderId, ""); // 返回成功
    }

    // ══════════════════════════════════════════════════════════════
    // POST /admin/api/wepay/v3/email/resend — 管理端手动重发邮件
    // ══════════════════════════════════════════════════════════════
    // 管理端手动重发支付失败邮件，Body: {order_id}
    void emailResend(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 检查请求体
        std::string orderId = (*body).get("order_id", "").asString(); // 获取订单号
        if (orderId.empty()) { RESP_ERR(cb, "order_id 必填"); return; } // 检查订单号

        auto &db    = PayDb::instance(); // 获取数据库实例
        auto order  = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId}); // 查询订单
        if (order.empty()) { RESP_ERR(cb, "订单不存在"); return; } // 订单不存在

        std::string mchId = order["mch_id"]; // 获取商户 ID
        auto mch  = db.queryOne("SELECT notify_email FROM merchant WHERE id=?", {mchId}); // 查询商户邮箱
        std::string toEmail = mch.count("notify_email") ? mch.at("notify_email") : ""; // 获取邮箱
        if (toEmail.empty()) { RESP_ERR(cb, "商户未配置通知邮箱"); return; } // 邮箱为空

        std::string baseUrl = db.getSetting("site_url", "http://localhost"); // 获取网站 URL
        WepayV3EmailService::instance().sendPayFail( // 发送支付失败邮件
            orderId, mchId, toEmail,
            order.count("real_amount") ? order.at("real_amount") : "0.00",
            "管理员手动重发", baseUrl);

        RESP_MSG(cb, "已重新发送邮件"); // 返回成功
    }

    // ══════════════════════════════════════════════════════════════
    // GET /admin/api/wepay/v3/email/logs — 邮件发送记录
    // ══════════════════════════════════════════════════════════════
    void emailLogs(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int page = 1, size = 20; // 初始化分页参数
        try { if (!req->getParameter("page").empty()) page = std::stoi(req->getParameter("page")); } catch (...) {} // 解析页码
        try { if (!req->getParameter("size").empty()) size = std::stoi(req->getParameter("size")); } catch (...) {} // 解析页面大小
        size = std::min(size, 100); // 限制最大页面大小
        int offset = (page - 1) * size; // 计算偏移量

        auto rows = PayDb::instance().query( // 查询邮件日志
            "SELECT * FROM v3_email_log ORDER BY created_at DESC LIMIT ? OFFSET ?",
            {std::to_string(size), std::to_string(offset)});

        Json::Value list(Json::arrayValue); // 创建日志列表
        for (auto &r : rows) { // 遍历每条日志
            Json::Value item; // 创建日志项
            for (auto &kv : r) item[kv.first] = kv.second; // 复制所有字段
            list.append(item); // 添加到列表
        }
        RESP_OK(cb, list); // 返回日志列表
    }

    // ══════════════════════════════════════════════════════════════
    // GET /admin/api/wepay/v3/callback/logs — 手动回调记录
    // ══════════════════════════════════════════════════════════════
    void callbackLogs(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int page = 1, size = 20; // 初始化分页参数
        try { if (!req->getParameter("page").empty()) page = std::stoi(req->getParameter("page")); } catch (...) {} // 解析页码
        try { if (!req->getParameter("size").empty()) size = std::stoi(req->getParameter("size")); } catch (...) {} // 解析页面大小
        size = std::min(size, 100); // 限制最大页面大小
        int offset = (page - 1) * size; // 计算偏移量

        auto rows = PayDb::instance().query( // 查询回调日志
            "SELECT * FROM v3_manual_callback_log ORDER BY created_at DESC LIMIT ? OFFSET ?",
            {std::to_string(size), std::to_string(offset)});

        Json::Value list(Json::arrayValue); // 创建日志列表
        for (auto &r : rows) { // 遍历每条日志
            Json::Value item; // 创建日志项
            for (auto &kv : r) item[kv.first] = kv.second; // 复制所有字段
            // 补充是否已过期
            long long exp = 0, now = std::time(nullptr); // 初始化过期时间
            try { exp = std::stoll(r.count("token_expire") ? r.at("token_expire") : "0"); } catch (...) {} // 解析过期时间
            item["expired"] = (now > exp); // 判断是否过期
            list.append(item); // 添加到列表
        }
        RESP_OK(cb, list); // 返回日志列表
    }

private:
    // 记录回调日志
    static void logCallback(const std::string &orderId, const std::string &mchId,
                            const std::string &token, bool ok, const std::string &detail,
                            const std::string &ip, const std::string &ua) {
        long long now = std::time(nullptr); // 获取当前时间
        if (!ok) { // 如果回调失败
            PayDb::instance().exec( // 插入失败记录
                "INSERT OR IGNORE INTO v3_manual_callback_log"
                "(order_id,mch_id,callback_token,token_expire,callback_status,"
                "callback_time,client_ip,user_agent,created_at) VALUES(?,?,?,0,2,?,?,?,?)",
                {orderId, mchId, token, std::to_string(now),
                 ip, ua.substr(0, 200), std::to_string(now)});
        }
        (void)detail; // 未使用的参数
    }

    // 返回 HTML 响应
    static void htmlResp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                         bool success, const std::string &orderId, const std::string &msg) {
        std::string body; // HTML 响应体
        if (success) { // 如果成功
            body = R"(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>回调成功</title>
<style>body{font-family:Arial;text-align:center;padding:50px;background:#f5f5f5}
.box{max-width:400px;margin:0 auto;background:#fff;border-radius:8px;padding:40px;box-shadow:0 2px 8px rgba(0,0,0,.1)}
.ic{font-size:64px}.h{color:#28a745}.p{color:#666;margin-top:10px}</style>
</head><body><div class="box"><div class="ic">&#x2705;</div>
<h1 class="h">回调成功</h1>
<p class="p">订单回调已成功触发，请检查您的服务器是否收到通知。</p>
<p class="p">订单号: )" + orderId + R"(</p></div></body></html>)"; // 成功页面
        } else { // 如果失败
            body = R"(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>回调失败</title>
<style>body{font-family:Arial;text-align:center;padding:50px;background:#f5f5f5}
.box{max-width:400px;margin:0 auto;background:#fff;border-radius:8px;padding:40px;box-shadow:0 2px 8px rgba(0,0,0,.1)}
.ic{font-size:64px}.h{color:#dc3545}.p{color:#666;margin-top:10px}</style>
</head><body><div class="box"><div class="ic">&#x274C;</div>
<h1 class="h">回调失败</h1>
<p class="p">)" + (msg.empty() ? "订单回调触发失败，请联系技术支持。" : msg) + R"(</p>)" +
(orderId.empty() ? "" : "<p class=\"p\">订单号: " + orderId + "</p>") +
R"(</div></body></html>)"; // 失败页面
        }
        auto resp = drogon::HttpResponse::newHttpResponse(); // 创建 HTTP 响应
        resp->setContentTypeCode(drogon::CT_TEXT_HTML); // 设置内容类型为 HTML
        resp->setBody(body); // 设置响应体
        cb(resp); // 返回响应
    }
};
