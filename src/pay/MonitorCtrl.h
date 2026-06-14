// 监控端控制器 — 支持三种监控协议
// 已知三种监控协议：
//   V 免签:        /appHeart, /appPush  (sign = MD5(t+key) / MD5(type+price+t+key))
//   码支付 v1:    /checkOrder/{pid}/{sign}, /checkPayResult, /mpayNotify
//   新版 API:     /api/monitor/heart, /api/monitor/push
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式库
#include <regex> // 正则表达式库
#include <json/json.h> // JSON 库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/Md5Utils.h" // MD5 和签名工具
#include "../common/EpaySign.h" // 易支付签名工具
#include "../common/HttpCaller.h" // HTTP 调用工具
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../common/WsBus.h" // WebSocket 消息总线
#include "../channel/VmqPlugin.h" // V 免签插件
#include "../channel/CodePayPlugin.h" // 码支付插件

// 监控端控制器类
class MonitorCtrl : public drogon::HttpController<MonitorCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        // V 免签协议
        ADD_METHOD_TO(MonitorCtrl::appHeart,      "/appHeart",                drogon::Post, drogon::Get); // 心跳路由
        ADD_METHOD_TO(MonitorCtrl::appPush,       "/appPush",                 drogon::Post, drogon::Get); // 推送路由
        // 新版 API
        ADD_METHOD_TO(MonitorCtrl::apiHeart,      "/api/monitor/heart",       drogon::Post); // API 心跳路由
        ADD_METHOD_TO(MonitorCtrl::apiPush,       "/api/monitor/push",        drogon::Post); // API 推送路由
        // 码支付 v1 监控端
        ADD_METHOD_TO(MonitorCtrl::checkOrder,    "/checkOrder/{pid}/{sign}", drogon::Get); // 查询订单路由
        ADD_METHOD_TO(MonitorCtrl::checkPayResult,"/checkPayResult",          drogon::Get, drogon::Post); // 查询支付结果路由
        ADD_METHOD_TO(MonitorCtrl::mpayNotify,    "/mpayNotify",              drogon::Post); // 支付通知路由
        // 多租户独立监听端：每商户独立路径 + 独立 vmq_key + 订单池隔离
        ADD_METHOD_TO(MonitorCtrl::mAppHeart,     "/m/{mch_no}/appHeart", drogon::Post, drogon::Get); // 多租户心跳路由
        ADD_METHOD_TO(MonitorCtrl::mAppPush,      "/m/{mch_no}/appPush",  drogon::Post, drogon::Get); // 多租户推送路由
    METHOD_LIST_END // 路由列表结束

    // ═══════════════════════════════════════════════════════════════
    // V 免签心跳（监控端定期上报在线状态）
    // ═══════════════════════════════════════════════════════════════
    void appHeart(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string t    = getParam(req, "t"); // 时间戳
        std::string sign = getParam(req, "sign"); // 签名
        LOG_INFO << "[appHeart] from=" << req->getPeerAddr().toIpPort() // 记录请求来源
                 << " t=" << t << " sign=" << sign;
        if (t.empty() || sign.empty()) { legacyResp(cb, -1, "缺少必要参数"); return; } // 检查参数
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string key = db.getSetting("key"); // 获取系统密钥
        if (!VmqPlugin::handleHeart(t, sign, key)) { // 处理心跳
            LOG_WARN << "[appHeart] sign mismatch t=" << t << " platform_key=" << key; // 签名不匹配
            legacyResp(cb, -1, "密钥错误，请检查配置"); return; // 返回错误
        }
        // V 免签独立维护自己的心跳，不再同步 wepay_*
        LOG_INFO << "[appHeart] OK"; // 记录成功
        legacyResp(cb, 1, "成功"); // 返回成功
    }

    // API 心跳（新版 API）
    void apiHeart(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string t    = getParam(req, "t"); // 时间戳
        std::string sign = getParam(req, "sign"); // 签名
        if (t.empty() || sign.empty()) { RESP_ERR(cb, "缺少必要参数"); return; } // 检查参数
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string key = db.getSetting("key"); // 获取系统密钥
        if (!VmqPlugin::handleHeart(t, sign, key)) { RESP_ERR(cb, "密钥错误"); return; } // 处理心跳
        RESP_MSG(cb, "心跳更新成功"); // 返回成功
    }

    // ═══════════════════════════════════════════════════════════════
    // V 免签推送收款（监控端上报收款信息）
    // ═══════════════════════════════════════════════════════════════
    void appPush(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string t     = getParam(req, "t"); // 时间戳
        std::string type  = getParam(req, "type"); // 支付类型
        std::string price = getParam(req, "price"); // 金额
        std::string sign  = getParam(req, "sign"); // 签名
        LOG_INFO << "[appPush] from=" << req->getPeerAddr().toIpPort() // 记录请求来源
                 << " t=" << t << " type=" << type << " price=" << price
                 << " sign=" << sign;
        if (t.empty() || type.empty() || price.empty() || sign.empty()) { // 检查参数
            legacyResp(cb, -1, "缺少必要参数"); return; // 返回错误
        }
        std::string priceFmt = fmtPrice(price); // 格式化金额
        int rc = VmqPlugin::handlePaymentPushEx(type, priceFmt, "", t, sign); // 处理支付推送
        LOG_INFO << "[appPush] rc=" << rc << " (type=" << type // 记录处理结果
                 << " priceFmt=" << priceFmt << ")";
        if (rc < 0) { legacyResp(cb, -1, "签名校验不通过"); return; } // 签名错误
        legacyResp(cb, 1, rc == 1 ? "成功" : "已收到，但未匹配到订单"); // 返回结果
    }

    // API 推送（新版 API）
    void apiPush(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string t     = getParam(req, "t"); // 时间戳
        std::string type  = getParam(req, "type"); // 支付类型
        std::string price = getParam(req, "price"); // 金额
        std::string sign  = getParam(req, "sign"); // 签名
        if (t.empty() || type.empty() || price.empty() || sign.empty()) { // 检查参数
            RESP_ERR(cb, "缺少必要参数"); return; // 返回错误
        }
        std::string priceFmt = fmtPrice(price); // 格式化金额
        int rc = VmqPlugin::handlePaymentPushEx(type, priceFmt, "", t, sign); // 处理支付推送
        if (rc < 0) { RESP_ERR(cb, "签名错误"); return; } // 签名错误
        RESP_MSG(cb, rc == 1 ? "推送成功" : "已收到，但未匹配到订单"); // 返回结果
    }

    // ── 码支付 v1: GET /checkOrder/{pid}/{sign} ────────────────
    // pid = merchant.mch_no
    // sign = md5(pid + merchant.mch_key)
    // 也支持 pid = "0" / "platform" 表示平台级（用平台 key, 返回所有 codepay 订单）
    void checkOrder(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    const std::string &pid, const std::string &sign) {
        auto &db = PayDb::instance();
        int mchId = 0;
        std::string key;
        if (pid == "0" || pid == "platform") {
            key = db.getSetting("key", "");
        } else {
            auto mch = db.queryOne("SELECT id,mch_key FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (mch.empty()) {
                Json::Value r; r["code"]=2; r["msg"]="商户号不存在";
                cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
            }
            try { mchId = std::stoi(mch["id"]); } catch (...) {}
            key = mch["mch_key"];
        }
        if (key.empty() || Md5Utils::md5(pid + key) != sign) {
            Json::Value r; r["code"]=2; r["msg"]="签名错误";
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }
        // 心跳: 写入码支付专属字段(独立于 V免签)
        long long now = std::time(nullptr);
        if (mchId > 0) {
            db.exec("UPDATE merchant SET codepay_last_heart=?,codepay_state=1 WHERE id=?",
                    {std::to_string(now), std::to_string(mchId)});
        } else {
            db.setSetting("codepay_lastheart", std::to_string(now));
            db.setSetting("codepay_jkstate",   "1");
        }

        Json::Value r;
        Json::Value orders = CodePayPlugin::pendingCodepayOrders(mchId);
        if (orders.empty()) { r["code"]=0; r["msg"]="没有新订单"; }
        else {
            r["code"]=1; r["msg"]="有"+std::to_string(orders.size())+"个待支付订单";
            r["orders"] = orders;
        }
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ── 码支付 v1: GET|POST /checkPayResult ────
    // 参数: pid, price, type/payway, [aid, sign]
    // 仅匹配 codepay 通道的订单, 与 V免签 /appPush 隔离
    void checkPayResult(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string pid    = getParam(req, "pid");
        std::string price  = getParam(req, "price");
        std::string type   = getParam(req, "type");
        if (type.empty()) type = getParam(req, "payway");
        if (price.empty() || type.empty()) {
            Json::Value r; r["code"]=2; r["msg"]="参数错误";
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }
        auto &db = PayDb::instance();
        int mchId = 0;
        if (!pid.empty() && pid != "0" && pid != "platform") {
            auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (mch.empty()) {
                Json::Value r; r["code"]=2; r["msg"]="商户号错误";
                cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
            }
            try { mchId = std::stoi(mch["id"]); } catch (...) {}
        }
        price = fmtPrice(price);
        long long now = std::time(nullptr);
        if (mchId > 0) {
            db.exec("UPDATE merchant SET codepay_last_pay=? WHERE id=?",
                    {std::to_string(now), std::to_string(mchId)});
        } else {
            db.setSetting("codepay_lastpay", std::to_string(now));
        }
        int rc = CodePayPlugin::handleCodepayPush(type, price,
                    mchId > 0 ? std::to_string(mchId) : "");
        Json::Value r;
        r["code"] = rc == 1 ? 1 : 0;
        r["msg"]  = rc == 1 ? "成功" : "已收到，但未匹配到订单";
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ── 码支付 v1: POST /mpayNotify ─────────────────────────────
    // 兼容三种格式:
    //  1. SmsForwarder 自定义模板: JSON body {pid,aid,title,msg,time,uid,divice}
    //  2. SmsForwarder 基础 webhook: JSON body {action,data(纯文本),time,sign}
    //  3. 旧格式: action=mpay&data={pid,price,payway}
    void mpayNotify(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();

        std::string pid, price, typeStr;

        auto body = req->getJsonObject();
        if (body && (*body).isMember("aid") && (*body).isMember("msg")) {
            // SmsForwarder 自定义模板
            pid = (*body).get("pid", "").asString();
            std::string aid = (*body).get("aid", "").asString();
            std::string msg = (*body).get("msg", "").asString();
            price = extractAmount(msg);
            if (price.empty()) { auto r = drogon::HttpResponse::newHttpResponse();
                r->setBody("无法从通知内容提取金额"); cb(r); return; }
            auto qr = db.queryOne("SELECT type FROM pay_qrcode WHERE id=?", {aid});
            if (qr.empty()) { auto r = drogon::HttpResponse::newHttpResponse();
                r->setBody("aid 错误"); cb(r); return; }
            typeStr = qr["type"];
        } else {
            std::string action = getParam(req, "action");
            std::string data   = getParam(req, "data");
            if (action != "mpay" && action != "mpaypc") {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setBody("非mpay请求"); cb(r); return;
            }
            Json::Value payload; Json::Reader reader;
            if (reader.parse(data, payload) && payload.isObject()) {
                pid = payload.get("pid", "").asString();
                price = payload.get("price", "").asString();
                std::string payway = payload.get("payway", "").asString();
                if (price.empty() || payway.empty()) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setBody("参数不完整"); cb(r); return; }
                int typeInt = EpaySign::typeToInt(payway);
                if (typeInt == 0) { auto r = drogon::HttpResponse::newHttpResponse();
                    r->setBody("payway错误"); cb(r); return; }
                typeStr = std::to_string(typeInt);
            } else {
                price = extractAmount(data);
                if (price.empty()) { auto r = drogon::HttpResponse::newHttpResponse();
                    r->setBody("无法从通知内容提取金额"); cb(r); return; }
                typeStr = "1";
            }
        }

        price = fmtPrice(price);
        // pid → 查系统商户表: "0"/"platform"/空 = 平台级, 其他 = 按 mch_no 查商户
        std::string mchIdStr;
        long long now = std::time(nullptr);
        if (!pid.empty() && pid != "0" && pid != "platform") {
            auto mch = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (mch.empty()) {
                // 也尝试按商户 id 查
                auto mch2 = db.queryOne("SELECT id FROM merchant WHERE id=? AND state=1", {pid});
                if (mch2.empty()) {
                    LOG_WARN << "[mpayNotify] pid=" << pid << " 未匹配到商户，视为平台级";
                } else {
                    mchIdStr = mch2["id"];
                }
            } else {
                mchIdStr = mch["id"];
            }
            if (!mchIdStr.empty()) {
                db.exec("UPDATE merchant SET codepay_last_pay=? WHERE id=?",
                        {std::to_string(now), mchIdStr});
            }
        }
        if (mchIdStr.empty()) db.setSetting("codepay_lastpay", std::to_string(now));

        // 先处理订单，再返回结果（安卓端根据返回体判定成功/失败）
        int rc = CodePayPlugin::handleCodepayPush(typeStr, price, mchIdStr);
        Json::Value resp;
        resp["code"] = rc == 1 ? 200 : 0;
        resp["msg"]  = rc == 1 ? "成功" : "已收到，但未匹配到订单";
        cb(drogon::HttpResponse::newHttpJsonResponse(resp));
    }

    // ══════════════════════════════════════════════════════════
    //  多租户路径 (V免签 协议 / 商户独立 vmq_key + 独立订单池)
    //    GET/POST /m/{mch_no}/appHeart?t=&sign=
    //      sign = md5(t + merchant.vmq_key)
    //    GET/POST /m/{mch_no}/appPush?t=&type=&price=&sign=
    //      sign = md5(type + price + t + merchant.vmq_key)
    // ══════════════════════════════════════════════════════════
    void mAppHeart(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                   const std::string &mchNo) {
        std::string t    = getParam(req, "t");
        std::string sign = getParam(req, "sign");
        LOG_INFO << "[m/appHeart] mch_no=" << mchNo << " t=" << t << " sign=" << sign;
        if (t.empty() || sign.empty()) { legacyResp(cb, -1, "缺少必要参数"); return; }
        auto &db = PayDb::instance();
        auto m = db.queryOne("SELECT id,vmq_key FROM merchant WHERE mch_no=? AND state=1", {mchNo});
        if (m.empty()) { legacyResp(cb, -1, "商户不存在或已禁用"); return; }
        std::string key = m["vmq_key"];
        if (key.empty()) { legacyResp(cb, -1, "商户未配置 vmq_key, 请到管理端生成"); return; }
        if (Md5Utils::heartSign(t, key) != sign) {
            legacyResp(cb, -1, "密钥错误"); return;
        }
        long long now = std::time(nullptr);
        db.exec("UPDATE merchant SET vmq_last_heart=?,vmq_state=1 WHERE id=?",
                {std::to_string(now), m["id"]});
        legacyResp(cb, 1, "成功");
    }

    void mAppPush(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                  const std::string &mchNo) {
        std::string t     = getParam(req, "t");
        std::string type  = getParam(req, "type");
        std::string price = getParam(req, "price");
        std::string sign  = getParam(req, "sign");
        LOG_INFO << "[m/appPush] mch_no=" << mchNo << " t=" << t
                 << " type=" << type << " price=" << price;
        if (t.empty() || type.empty() || price.empty() || sign.empty()) {
            legacyResp(cb, -1, "缺少必要参数"); return;
        }
        auto &db = PayDb::instance();
        auto m = db.queryOne("SELECT id,vmq_key,mch_key FROM merchant WHERE mch_no=? AND state=1", {mchNo});
        if (m.empty()) { legacyResp(cb, -1, "商户不存在或已禁用"); return; }
        std::string key = m["vmq_key"];
        if (key.empty() || Md5Utils::pushSign(type, price, t, key) != sign) {
            legacyResp(cb, -1, "签名校验不通过"); return;
        }
        std::string priceFmt = fmtPrice(price);
        long long now = std::time(nullptr);
        db.exec("UPDATE merchant SET vmq_last_pay=? WHERE id=?", {std::to_string(now), m["id"]});

        // 商户级订单池匹配 (隔离, 不抢其他商户订单)
        auto order = db.queryOne(
            "SELECT * FROM pay_order "
            "WHERE state=0 AND mch_id=? AND LOWER(pay_type)=LOWER(?) "
            "AND (ABS(CAST(real_amount AS REAL)-?)<0.001 OR ABS(CAST(amount AS REAL)-?)<0.001) "
            "ORDER BY created_at ASC LIMIT 1",
            {m["id"], typeToPayType(type), priceFmt, priceFmt});
        if (order.empty()) {
            LOG_INFO << "[m/appPush] mch_no=" << mchNo << " no matching order";
            legacyResp(cb, 1, "已收到，但未匹配到订单"); return;
        }
        // 沿用 VmqPlugin 的 markOrderPaid 逻辑：用商户 mch_key 签回调
        VmqPlugin::markOrderPaidStatic(order, typeToPayType(type), m["mch_key"]);
        LOG_INFO << "[m/appPush] mch_no=" << mchNo << " matched order_id=" << order["order_id"];
        legacyResp(cb, 1, "成功");
    }

private:
    // 支付类型转换方法（数字转字符串）
    static std::string typeToPayType(const std::string &t) {
        if (t == "1") return "wxpay"; // 1 = 微信支付
        if (t == "2") return "alipay"; // 2 = 支付宝
        if (t == "3") return "qqpay"; // 3 = QQ 钱包
        return t; // 其他类型直接返回
    }

    // ═══════════════════════════════════════════════════════════════
    // 辅助方法
    // ═══════════════════════════════════════════════════════════════
    
    // 获取参数方法（支持 URL 参数和 JSON 请求体）
    static std::string getParam(const drogon::HttpRequestPtr &req, const std::string &k) {
        auto v = req->getParameter(k); // 先从 URL 参数获取
        if (!v.empty()) return v; // 如果找到则返回
        auto body = req->getJsonObject(); // 否则从 JSON 请求体获取
        if (body && (*body).isMember(k)) return (*body)[k].asString(); // 如果找到则返回
        return ""; // 都没找到则返回空字符串
    }
    
    // 格式化金额方法
    static std::string fmtPrice(const std::string &s) {
        try { double v = std::stod(s); std::ostringstream o; // 转换为浮点数
              o << std::fixed << std::setprecision(2) << v; return o.str(); // 保留 2 位小数
        } catch (...) { return s; } // 转换失败则返回原值
    }
    
    // 从短信内容里抽取金额（支持 "¥12.34" / "12.34元" / "收款 1.23" 等）
    static std::string extractAmount(const std::string &text) {
        std::regex re(R"((\d+\.\d{1,2})(?:\s*元|¥)?)"); // 正则表达式匹配金额
        std::smatch m; // 匹配结果
        if (std::regex_search(text, m, re)) return m[1].str(); // 找到则返回金额
        return ""; // 没找到则返回空字符串
    }
    
    // URL 编码方法
    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss; // 创建字符串流
        for (unsigned char c : s) { // 遍历每个字符
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c; // 保留安全字符
            else oss << '%' << std::uppercase << std::hex // 其他字符转换为 %HH 格式
                     << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str(); // 返回编码后的字符串
    }
    
    // 旧版响应方法（返回 JSON）
    static void legacyResp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                           int code, const std::string &msg) {
        Json::Value r; r["code"] = code; r["msg"] = msg; // 创建响应
        cb(drogon::HttpResponse::newHttpJsonResponse(r)); // 返回 JSON 响应
    }

    // ═══════════════════════════════════════════════════════════════
    // WePay 兼容层: 让老 V 免签/monitor 端点也能驱动 wepay 插件
    // ═══════════════════════════════════════════════════════════════
    
    // 同步 WePay 推送状态
    static void syncWepayPush(PayDb &db, const std::string &ip) {
        long long now = std::time(nullptr); // 获取当前时间
        db.setSetting("wepay_lastpay", std::to_string(now)); // 更新最后推送时间
        std::string deviceId = "ip_" + ip; // 生成设备 ID
        db.exec("UPDATE monitor_device SET last_push=?,push_count=push_count+1 WHERE device_id=?",
                {std::to_string(now), deviceId}); // 更新设备推送信息
    }

    // 同步 WePay 心跳状态
    static void syncWepayHeart(PayDb &db, const std::string &ip) {
        long long now = std::time(nullptr); // 获取当前时间
        db.setSetting("wepay_lastheart", std::to_string(now)); // 更新最后心跳时间
        db.setSetting("wepay_jkstate", "1"); // 标记为在线
        autoRegisterDevice(db, "ip_" + ip, ip, "旧版监控端"); // 自动注册设备
    }

    // 自动注册设备方法
    static void autoRegisterDevice(PayDb &db, const std::string &deviceId,
                                   const std::string &ip,
                                   const std::string &name = "") {
        long long now = std::time(nullptr); // 获取当前时间
        auto existing = db.queryOne("SELECT id FROM monitor_device WHERE device_id=?", {deviceId}); // 查询设备是否存在
        if (existing.empty()) { // 如果不存在
            db.exec("INSERT INTO monitor_device(device_id,device_name,device_model,ip,state,last_heart,created_at)"
                    " VALUES(?,?,?,?,1,?,?)",
                    {deviceId, name, "legacy", ip, std::to_string(now), std::to_string(now)}); // 插入新设备
        } else { // 如果存在
            db.exec("UPDATE monitor_device SET last_heart=?,ip=? WHERE device_id=?",
                    {std::to_string(now), ip, deviceId}); // 更新设备信息
        }
    }
};
