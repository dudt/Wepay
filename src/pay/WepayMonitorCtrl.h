// WePay 原生监控协议 — 专属控制器
// 与 vmq/codepay 的核心区别:
//   1. HMAC-SHA256 签名（不是 MD5）
//   2. 时间戳防重放（60 秒窗口）
//   3. Nonce 去重（防止同一请求重复提交）
//   4. 设备注册/绑定（未注册设备拒绝推送）
//   5. 订单级推送（精确 order_id 匹配，不再只靠金额模糊匹配）
//   6. 推送回执（返回匹配到的订单号和金额）
//   7. 待支付列表拉取（监控端主动拉取待支付订单）
//
// 端点:
//   POST /api/wepay/heart    — 心跳 + 设备注册
//   POST /api/wepay/push     — 收款推送（带 order_id 精确匹配）
//   POST /api/wepay/pending  — 拉取待支付订单列表
//   GET  /admin/api/wepay/devices — 管理端查看已注册设备
//   POST /admin/api/wepay/device/kick — 踢下线
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式库
#include <mutex> // 互斥锁
#include <unordered_set> // 哈希集合
#include <json/json.h> // JSON 库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/Md5Utils.h" // MD5 和签名工具
#include "../common/EpaySign.h" // 易支付签名工具
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../common/WsBus.h" // WebSocket 消息总线
#include "../channel/VmqPlugin.h" // V 免签插件
#include "../channel/CodePayPlugin.h" // 码支付插件

// WePay 原生监控协议控制器类
class WepayMonitorCtrl : public drogon::HttpController<WepayMonitorCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(WepayMonitorCtrl::heart,    "/api/wepay/heart",    drogon::Post); // 心跳路由
        ADD_METHOD_TO(WepayMonitorCtrl::push,     "/api/wepay/push",     drogon::Post); // 推送路由
        ADD_METHOD_TO(WepayMonitorCtrl::pending,  "/api/wepay/pending",  drogon::Post); // 待支付订单查询路由
        ADD_METHOD_TO(WepayMonitorCtrl::devices,  "/admin/api/wepay/devices",    drogon::Get); // 设备列表路由
        ADD_METHOD_TO(WepayMonitorCtrl::kickDevice,   "/admin/api/wepay/device/kick",   drogon::Post); // 踢下线路由
        ADD_METHOD_TO(WepayMonitorCtrl::deleteDevice, "/admin/api/wepay/device/delete", drogon::Post); // 删除设备路由
    METHOD_LIST_END // 路由列表结束

    // ══════════════════════════════════════════════════════════════
    // POST /api/wepay/heart — 设备心跳（上报在线状态）
    // ══════════════════════════════════════════════════════════════
    // 请求体: { t, device_id, device_name?, device_model?, nonce, sign, pid? }
    // 签名: hmac_sha256(key, "heart\nt\ndevice_id\nnonce")
    // pid 可选: 有则用商户 vmq_key 签名，无则用平台 key
    void heart(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { resp(cb, 400, "请求体必须是JSON"); return; } // 检查请求体
        auto &j = *body; // 获取 JSON 对象引用
        std::string t        = j.get("t", "").asString(); // 时间戳
        std::string deviceId = j.get("device_id", "").asString(); // 设备 ID
        std::string nonce    = j.get("nonce", "").asString(); // 随机数
        std::string sign     = j.get("sign", "").asString(); // 签名

        if (t.empty() || deviceId.empty() || nonce.empty() || sign.empty()) { // 检查必要参数
            resp(cb, 400, "缺少必要参数 (t, device_id, nonce, sign)"); return; // 参数不完整
        }

        // 1) 时间戳防重放（防止重放攻击）
        if (!checkTimestamp(t)) { // 检查时间戳是否有效
            resp(cb, 403, "时间戳过期，请检查设备时间"); return; // 时间戳过期
        }
        // 2) Nonce 去重（防止重复请求）
        if (!consumeNonce(nonce)) { // 检查 nonce 是否已使用
            resp(cb, 403, "nonce 重复，疑似重放攻击"); return; // Nonce 已使用
        }
        // 3) HMAC-SHA256 签名校验
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string pid = j.get("pid", "").asString(); // 商户号
        std::string key; // 签名密钥
        std::string mchId;  // 商户 ID（非空=商户级）
        if (!pid.empty()) { // 如果提供了商户号
            auto m = db.queryOne("SELECT id,vmq_key FROM merchant WHERE mch_no=? AND state=1", {pid}); // 查询商户
            if (m.empty()) { resp(cb, 403, "商户不存在或已禁用"); return; } // 商户不存在
            key = m["vmq_key"]; // 获取商户密钥
            mchId = m["id"]; // 获取商户 ID
            if (key.empty()) { resp(cb, 403, "商户未配置 vmq_key"); return; } // 商户未配置密钥
        } else { // 否则使用平台密钥
            key = db.getSetting("key"); // 获取平台密钥
        }
        std::string signMsg = "heart\n" + t + "\n" + deviceId + "\n" + nonce; // 构建签名消息
        std::string expected = Md5Utils::wepayHeartSign(t, deviceId, nonce, key); // 计算期望签名
        LOG_INFO << "[wepay/heart] DEBUG key=" << key // 记录调试信息
                 << " msg=[" << signMsg << "]"
                 << " expected=" << expected << " got=" << sign;
        if (sign != expected) { // 验证签名
            LOG_WARN << "[wepay/heart] sign mismatch device=" << deviceId // 签名不匹配
                     << " expected=" << expected << " got=" << sign;
            resp(cb, 403, "签名错误"); return; // 返回错误
        }
        // 4) 注册/更新设备
        std::string ip = req->getPeerAddr().toIp(); // 获取请求 IP
        std::string deviceName  = j.get("device_name", "").asString(); // 设备名称
        std::string deviceModel = j.get("device_model", "").asString(); // 设备型号
        long long now = std::time(nullptr); // 获取当前时间

        auto existing = db.queryOne("SELECT id,state FROM monitor_device WHERE device_id=?", {deviceId}); // 查询设备是否存在
        if (existing.empty()) { // 设备不存在
            db.exec("INSERT INTO monitor_device(device_id,device_name,device_model,ip,state,last_heart,created_at)"
                    " VALUES(?,?,?,?,1,?,?)",
                    {deviceId, deviceName, deviceModel, ip, std::to_string(now), std::to_string(now)}); // 插入新设备
            LOG_INFO << "[wepay/heart] new device registered: " << deviceId; // 记录新设备注册
        } else { // 设备已存在
            if (existing["state"] == "0") { // 检查设备是否被禁用
                resp(cb, 403, "设备已被管理员禁用"); return; // 设备被禁用
            }
            db.exec("UPDATE monitor_device SET last_heart=?,ip=?,device_name=?,device_model=? WHERE device_id=?",
                    {std::to_string(now), ip, deviceName, deviceModel, deviceId}); // 更新设备信息
        }
        // 5) 更新平台心跳（用于监控设备在线状态）
        db.setSetting("wepay_lastheart", std::to_string(now)); // 记录最后心跳时间
        db.setSetting("wepay_jkstate", "1"); // 标记为在线
        // 兼容 vmq 心跳字段（前端监控状态复用）
        db.setSetting("jkstate", "1"); // 兼容字段
        db.setSetting("lastheart", std::to_string(now)); // 兼容字段

        // 实时事件（通知前端设备状态）
        Json::Value hev; // 创建事件对象
        hev["device_id"] = deviceId; hev["device_name"] = deviceName; // 设备信息
        hev["ip"] = ip; hev["protocol"] = "wepay_v2"; // 协议标记
        WsBus::instance().publishLive("heartbeat", hev); // 发布心跳事件

        Json::Value data; // 创建响应
        data["code"] = 200; // 状态码
        data["msg"] = "成功"; // 消息
        data["server_time"] = (Json::Int64)now; // 服务器时间
        cb(drogon::HttpResponse::newHttpJsonResponse(data)); // 返回响应
    }

    // ══════════════════════════════════════════════════════════════
    //  POST /api/wepay/push
    //  Body: { t, type, price, order_id?, device_id, nonce, sign, pid? }
    //  sign = hmac_sha256(key, "push\ntype\nprice\nt\norder_id\nnonce")
    //  order_id 可选: 有则精确匹配，无则金额匹配（兼容模式）
    //  pid 可选: 有则用商户 key + 限定商户订单池
    // ══════════════════════════════════════════════════════════════
    void push(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { resp(cb, 400, "请求体必须是JSON"); return; }
        auto &j = *body;
        std::string t        = j.get("t", "").asString();
        std::string type     = j.get("type", "").asString();
        std::string price    = j.get("price", "").asString();
        std::string orderId  = j.get("order_id", "").asString();
        std::string deviceId = j.get("device_id", "").asString();
        std::string nonce    = j.get("nonce", "").asString();
        std::string sign     = j.get("sign", "").asString();

        if (t.empty() || type.empty() || price.empty() || deviceId.empty() ||
            nonce.empty() || sign.empty()) {
            resp(cb, 400, "缺少必要参数"); return;
        }

        // 1) 时间戳防重放
        if (!checkTimestamp(t)) { resp(cb, 403, "时间戳过期"); return; }
        // 2) Nonce 去重
        if (!consumeNonce(nonce)) { resp(cb, 403, "nonce 重复"); return; }
        // 3) 签名校验
        auto &db = PayDb::instance();
        std::string pid = j.get("pid", "").asString();
        std::string key;
        std::string mchId;
        std::string mchKey;  // 商户 mch_key(回调签名用)
        if (!pid.empty()) {
            auto m = db.queryOne("SELECT id,vmq_key,mch_key FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (m.empty()) { resp(cb, 403, "商户不存在或已禁用"); return; }
            key = m["vmq_key"]; mchId = m["id"]; mchKey = m["mch_key"];
            if (key.empty()) { resp(cb, 403, "商户未配置 vmq_key"); return; }
        } else {
            key = db.getSetting("key");
        }
        std::string expected = Md5Utils::wepayPushSign(type, price, t, orderId, nonce, key);
        if (sign != expected) {
            LOG_WARN << "[wepay/push] sign mismatch device=" << deviceId;
            resp(cb, 403, "签名错误"); return;
        }
        // 4) 设备校验
        auto dev = db.queryOne("SELECT id,state FROM monitor_device WHERE device_id=?", {deviceId});
        if (dev.empty()) { resp(cb, 403, "设备未注册，请先发送心跳"); return; }
        if (dev["state"] == "0") { resp(cb, 403, "设备已被禁用"); return; }

        // 5) 订单匹配
        long long now = std::time(nullptr);
        std::string priceFmt = fmtPrice(price);
        std::string payType = typeToPayType(type);
        PayDb::Row order;

        if (!orderId.empty()) {
            // 精确匹配: 用 order_id (有商户则限定商户)
            if (!mchId.empty()) {
                order = db.queryOne(
                    "SELECT * FROM pay_order WHERE order_id=? AND state=0 AND pay_type=? AND mch_id=?",
                    {orderId, payType, mchId});
            } else {
                order = db.queryOne(
                    "SELECT * FROM pay_order WHERE order_id=? AND state=0 AND pay_type=?",
                    {orderId, payType});
            }
            if (order.empty()) {
                LOG_WARN << "[wepay/push] order_id=" << orderId << " not found or state!=0";
            }
        }
        if (order.empty()) {
            // 金额兜底匹配 (有商户则限定商户订单池)
            if (!mchId.empty()) {
                order = db.queryOne(
                    "SELECT * FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) AND mch_id=? "
                    "AND (ABS(CAST(real_amount AS REAL)-?)<0.001 OR ABS(CAST(amount AS REAL)-?)<0.001) "
                    "ORDER BY created_at ASC LIMIT 1",
                    {payType, mchId, priceFmt, priceFmt});
            } else {
                order = CodePayPlugin::findPendingCodepayOrder(payType, priceFmt);
            }
        }

        // 6) 更新设备统计
        db.exec("UPDATE monitor_device SET last_push=?,push_count=push_count+1 WHERE device_id=?",
                {std::to_string(now), deviceId});
        db.setSetting("wepay_lastpay", std::to_string(now));
        if (!mchId.empty()) {
            db.exec("UPDATE merchant SET vmq_last_pay=? WHERE id=?", {std::to_string(now), mchId});
        }

        // 实时事件: push
        Json::Value pev;
        pev["device_id"] = deviceId; pev["type"] = type;
        pev["price"] = priceFmt; pev["protocol"] = "wepay_v2";
        if (!pid.empty()) pev["pid"] = pid;

        if (order.empty()) {
            pev["matched"] = false;
            WsBus::instance().publishLive("push", pev);
            Json::Value r;
            r["code"] = 200;
            r["msg"] = "成功";
            r["matched"] = false;
            r["detail"] = "已收到推送，但未匹配到待支付订单";
            cb(drogon::HttpResponse::newHttpJsonResponse(r));
            return;
        }

        // 7) 标记订单已付 (商户级用 mch_key 签回调, 平台级用平台 key)
        std::string callKey = !mchKey.empty() ? mchKey : db.getSetting("key");
        VmqPlugin::markOrderPaidStatic(order, payType, callKey);

        std::string matchedOrderId = order.count("order_id") ? order["order_id"] : "";
        std::string matchedAmount  = order.count("real_amount") ? order["real_amount"] : "";
        pev["matched"]  = true;
        pev["order_id"] = matchedOrderId;
        pev["amount"]   = matchedAmount;
        WsBus::instance().publishLive("push", pev);
        WsBus::instance().publishLive("order_paid", pev);

        Json::Value r;
        r["code"] = 200;
        r["msg"] = "成功";
        r["matched"] = true;
        r["order_id"] = matchedOrderId;
        r["amount"] = matchedAmount;
        LOG_INFO << "[wepay/push] device=" << deviceId << " matched order="
                 << matchedOrderId << " amount=" << priceFmt;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════════
    //  POST /api/wepay/pending
    //  Body: { t, device_id, nonce, sign }
    //  sign = hmac_sha256(key, "pending\nt\ndevice_id\nnonce")
    //  返回待支付订单列表（监控端可精确匹配 order_id）
    // ══════════════════════════════════════════════════════════════
    void pending(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { resp(cb, 400, "请求体必须是JSON"); return; }
        auto &j = *body;
        std::string t        = j.get("t", "").asString();
        std::string deviceId = j.get("device_id", "").asString();
        std::string nonce    = j.get("nonce", "").asString();
        std::string sign     = j.get("sign", "").asString();

        if (t.empty() || deviceId.empty() || nonce.empty() || sign.empty()) {
            resp(cb, 400, "缺少必要参数"); return;
        }
        if (!checkTimestamp(t)) { resp(cb, 403, "时间戳过期"); return; }
        if (!consumeNonce(nonce)) { resp(cb, 403, "nonce 重复"); return; }

        auto &db = PayDb::instance();
        std::string key = db.getSetting("key");
        std::string expected = Md5Utils::wepayPendingSign(t, deviceId, nonce, key);
        if (sign != expected) { resp(cb, 403, "签名错误"); return; }

        auto dev = db.queryOne("SELECT state FROM monitor_device WHERE device_id=?", {deviceId});
        if (dev.empty()) { resp(cb, 403, "设备未注册"); return; }
        if (dev["state"] == "0") { resp(cb, 403, "设备已被禁用"); return; }

        // 查询待支付订单
        auto rows = db.query(
            "SELECT po.order_id, po.pay_type, po.real_amount, po.created_at "
            "FROM pay_order po "
            "LEFT JOIN pay_channel pc ON pc.id=po.channel_id "
            "WHERE po.state=0 AND (pc.plugin IN ('wepay','wepay_v3','codepay','vmq','monitor',"
            "'wx_monitor','alipay_monitor','qq_monitor') "
            "OR po.channel_id=0 OR pc.plugin IS NULL) "
            "ORDER BY po.created_at ASC LIMIT 50", {});

        Json::Value arr(Json::arrayValue);
        for (auto &o : rows) {
            Json::Value item;
            item["order_id"]  = o.count("order_id") ? o["order_id"] : "";
            item["pay_type"]  = o.count("pay_type") ? o["pay_type"] : "";
            item["amount"]    = o.count("real_amount") ? o["real_amount"] : "";
            item["created_at"]= o.count("created_at") ? o["created_at"] : "";
            arr.append(item);
        }
        Json::Value r;
        r["code"] = 200;
        r["msg"] = arr.empty() ? "没有待支付订单" : "成功";
        r["count"] = (int)arr.size();
        r["orders"] = arr;
        r["server_time"] = (Json::Int64)std::time(nullptr);
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════════
    // GET /admin/api/wepay/devices — 管理端查看已注册的监控设备
    // ══════════════════════════════════════════════════════════════
    void devices(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query( // 查询所有设备
            "SELECT * FROM monitor_device ORDER BY last_heart DESC LIMIT 100", {}); // 按最后心跳时间排序
        long long now = std::time(nullptr); // 获取当前时间
        Json::Value arr(Json::arrayValue); // 创建设备列表
        for (auto &d : rows) { // 遍历每个设备
            Json::Value v; // 创建设备项
            v["id"]           = d.count("id") ? d["id"] : ""; // 设备数据库 ID
            v["device_id"]    = d.count("device_id") ? d["device_id"] : ""; // 设备 ID
            v["device_name"]  = d.count("device_name") ? d["device_name"] : ""; // 设备名称
            v["device_model"] = d.count("device_model") ? d["device_model"] : ""; // 设备型号
            v["ip"]           = d.count("ip") ? d["ip"] : ""; // IP 地址
            v["state"]        = d.count("state") ? d["state"] : "1"; // 设备状态
            v["last_heart"]   = d.count("last_heart") ? d["last_heart"] : "0"; // 最后心跳时间
            v["last_push"]    = d.count("last_push") ? d["last_push"] : "0"; // 最后推送时间
            v["push_count"]   = d.count("push_count") ? d["push_count"] : "0"; // 推送次数
            v["created_at"]   = d.count("created_at") ? d["created_at"] : "0"; // 创建时间
            long long lh = 0; // 初始化最后心跳时间
            try { lh = std::stoll(d.count("last_heart") ? d["last_heart"] : "0"); } catch (...) {} // 解析最后心跳时间
            v["online"] = (now - lh < 180) ? true : false; // 判断设备是否在线（180 秒内有心跳）
            arr.append(v); // 添加到列表
        }
        Json::Value data; // 创建响应
        data["code"] = 200; // 状态码
        data["data"] = arr; // 设备列表
        cb(drogon::HttpResponse::newHttpJsonResponse(data)); // 返回响应
    }

    // ══════════════════════════════════════════════════════════════
    // POST /admin/api/wepay/device/kick — 禁用/启用设备（切换状态）
    // ══════════════════════════════════════════════════════════════
    void kickDevice(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { RESP_ERR(cb, "参数错误"); return; } // 检查请求体
        std::string deviceId = (*body).get("device_id", "").asString(); // 获取设备 ID
        if (deviceId.empty()) { RESP_ERR(cb, "device_id 必填"); return; } // 检查设备 ID
        auto &db = PayDb::instance(); // 获取数据库实例
        auto dev = db.queryOne("SELECT id,state FROM monitor_device WHERE device_id=?", {deviceId}); // 查询设备
        if (dev.empty()) { RESP_ERR(cb, "设备不存在"); return; } // 设备不存在
        int newState = (dev["state"] == "1") ? 0 : 1; // 切换状态（1=启用，0=禁用）
        db.exec("UPDATE monitor_device SET state=? WHERE device_id=?",
                {std::to_string(newState), deviceId}); // 更新设备状态
        // 实时事件（通知前端设备状态变化）
        Json::Value kev; // 创建事件对象
        kev["device_id"] = deviceId; // 设备 ID
        kev["state"] = std::to_string(newState); // 新状态
        WsBus::instance().publishLive("device_kick", kev); // 发布事件
        RESP_MSG(cb, newState == 1 ? "设备已启用" : "设备已禁用"); // 返回消息
    }

    // POST /admin/api/wepay/device/delete — 删除设备
    void deleteDevice(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { RESP_ERR(cb, "参数错误"); return; } // 检查请求体
        std::string deviceId = (*body).get("device_id", "").asString(); // 获取设备 ID
        if (deviceId.empty()) { RESP_ERR(cb, "device_id 必填"); return; } // 检查设备 ID
        auto &db = PayDb::instance(); // 获取数据库实例
        auto dev = db.queryOne("SELECT id FROM monitor_device WHERE device_id=?", {deviceId}); // 查询设备
        if (dev.empty()) { RESP_ERR(cb, "设备不存在"); return; } // 设备不存在
        db.exec("DELETE FROM monitor_device WHERE device_id=?", {deviceId}); // 删除设备
        Json::Value ev; // 创建事件对象
        ev["device_id"] = deviceId; // 设备 ID
        WsBus::instance().publishLive("device_delete", ev); // 发布删除事件
        RESP_MSG(cb, "设备已删除"); // 返回消息
    }

private:
    // ═══════════════════════════════════════════════════════════════
    // 时间戳校验（60 秒窗口，防止重放攻击）
    // ═══════════════════════════════════════════════════════════════
    static bool checkTimestamp(const std::string &tStr) {
        long long t = 0; // 初始化时间戳
        try { t = std::stoll(tStr); } catch (...) { return false; } // 解析时间戳
        // 支持秒和毫秒两种格式
        if (t > 9999999999LL) t /= 1000; // 如果是毫秒则转换为秒
        long long now = std::time(nullptr); // 获取当前时间
        return std::abs(now - t) <= 60; // 检查时间差是否在 60 秒内
    }

    // ═══════════════════════════════════════════════════════════════
    // Nonce 去重（内存 + 数据库双层防护）
    // ═══════════════════════════════════════════════════════════════
    static bool consumeNonce(const std::string &nonce) {
        if (nonce.empty() || nonce.size() > 64) return false; // 检查 nonce 有效性
        { // 内存层防护
            std::lock_guard<std::mutex> lk(nonceMtx()); // 加锁
            if (nonceSet().count(nonce)) return false; // 如果已使用则返回 false
            nonceSet().insert(nonce); // 添加到已使用集合
            // 内存缓存上限 10000，超过清空（依赖 DB 兜底）
            if (nonceSet().size() > 10000) nonceSet().clear(); // 防止内存溢出
        }
        auto &db = PayDb::instance(); // 获取数据库实例
        // DB 兜底: 如果 nonce 已存在则拒绝
        auto existing = db.queryOne("SELECT 1 FROM nonce_cache WHERE nonce=?", {nonce}); // 查询数据库
        if (!existing.empty()) return false; // 如果存在则返回 false
        long long now = std::time(nullptr); // 获取当前时间
        db.exec("INSERT INTO nonce_cache(nonce,created_at) VALUES(?,?)",
                {nonce, std::to_string(now)}); // 插入 nonce 到数据库
        // 顺便清理 5 分钟前的旧 nonce
        db.exec("DELETE FROM nonce_cache WHERE created_at<?",
                {std::to_string(now - 300)}); // 删除过期 nonce
        return true; // 返回成功
    }

    // 获取互斥锁（静态变量）
    static std::mutex &nonceMtx() { static std::mutex m; return m; }
    // 获取 nonce 集合（静态变量）
    static std::unordered_set<std::string> &nonceSet() {
        static std::unordered_set<std::string> s; return s;
    }

    // 支付类型转换（数字转字符串）
    static std::string typeToPayType(const std::string &t) {
        if (t == "1") return "wxpay"; // 1 = 微信支付
        if (t == "2") return "alipay"; // 2 = 支付宝
        if (t == "3") return "qqpay"; // 3 = QQ 钱包
        return t; // 其他类型直接返回
    }
    
    // 格式化金额（保留 2 位小数）
    static std::string fmtPrice(const std::string &s) {
        try { double v = std::stod(s); std::ostringstream o; // 转换为浮点数
              o << std::fixed << std::setprecision(2) << v; return o.str(); // 保留 2 位小数
        } catch (...) { return s; } // 转换失败则返回原值
    }
    
    // 返回响应（JSON 格式）
    static void resp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                     int code, const std::string &msg) {
        Json::Value r; r["code"] = code; r["msg"] = msg; // 创建响应
        cb(drogon::HttpResponse::newHttpJsonResponse(r)); // 返回 JSON 响应
    }
};
