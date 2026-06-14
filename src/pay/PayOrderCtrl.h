// 订单管理控制器（V 免签兼容格式）
// 支持订单创建、查询、检查和过期关闭等功能
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <random> // 随机数库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式库
#include <cmath> // 数学库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/Md5Utils.h" // MD5 和签名工具

// 订单管理控制器类
// POST /createOrder              创建订单（V 免签格式）
// GET  /checkOrder/{orderId}     轮询订单状态
// GET  /getOrder/{orderId}       获取订单完整信息
// POST /api/order/closeExpired   关闭超时订单（内部触发）
class PayOrderCtrl : public drogon::HttpController<PayOrderCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(PayOrderCtrl::createOrder,  "/createOrder",            drogon::Post); // 创建订单路由
        ADD_METHOD_TO(PayOrderCtrl::checkOrder,   "/checkOrder/{orderId}",   drogon::Get); // 检查订单状态路由
        ADD_METHOD_TO(PayOrderCtrl::getOrder,     "/getOrder/{orderId}",     drogon::Get); // 获取订单信息路由
        ADD_METHOD_TO(PayOrderCtrl::closeExpired, "/api/order/closeExpired", drogon::Post); // 关闭过期订单路由
    METHOD_LIST_END // 路由列表结束

    // 创建订单方法
    void createOrder(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        closeExpiredInternal(); // 先关闭过期订单

        auto body = req->getJsonObject(); // 获取 JSON 请求体
        std::string payId, param, type, price, sign, notifyUrl, returnUrl; // 定义参数变量
        if (body) { // 如果有 JSON 请求体
            payId     = (*body).get("payId",     "").asString(); // 获取商户订单号
            param     = (*body).get("param",     "").asString(); // 获取额外参数
            type      = (*body).get("type",      "").asString(); // 获取支付类型
            price     = (*body).get("price",     "").asString(); // 获取金额
            sign      = (*body).get("sign",      "").asString(); // 获取签名
            notifyUrl = (*body).get("notifyUrl", "").asString(); // 获取异步通知 URL
            returnUrl = (*body).get("returnUrl", "").asString(); // 获取同步返回 URL
        } else { // 否则从 URL 参数获取
            payId     = req->getParameter("payId"); // 获取商户订单号
            param     = req->getParameter("param"); // 获取额外参数
            type      = req->getParameter("type"); // 获取支付类型
            price     = req->getParameter("price"); // 获取金额
            sign      = req->getParameter("sign"); // 获取签名
            notifyUrl = req->getParameter("notifyUrl"); // 获取异步通知 URL
            returnUrl = req->getParameter("returnUrl"); // 获取同步返回 URL
        }

        if (payId.empty() || type.empty() || price.empty() || sign.empty()) { // 检查必要参数
            RESP_ERR(cb, "参数不完整"); return; // 返回错误
        }
        if (type != "1" && type != "2") { // 检查支付类型（1=微信，2=支付宝）
            RESP_ERR(cb, "支付类型错误，1=微信 2=支付宝"); return; // 返回错误
        }
        double priceVal = 0; // 初始化金额
        try { priceVal = std::stod(price); } catch (...) { RESP_ERR(cb, "金额格式错误"); return; } // 解析金额
        if (priceVal <= 0) { RESP_ERR(cb, "价格错误"); return; } // 检查金额是否大于 0

        auto &db = PayDb::instance(); // 获取数据库实例
        std::string key = db.getSetting("key"); // 获取系统密钥
        if (key.empty()) { RESP_ERR(cb, "系统未配置通讯密钥"); return; } // 检查密钥
        if (Md5Utils::orderSign(payId, param, type, price, key) != sign) { // 验证签名
            RESP_ERR(cb, "签名错误"); return; // 签名错误
        }
        if (db.getSetting("jkstate") != "1") { // 检查监控端状态
            RESP_ERR(cb, "监控端离线，请检查手机/PC监听软件"); return; // 监控端离线
        }
        auto existing = db.queryOne("SELECT id FROM pay_order WHERE mch_order_no=?", {payId}); // 查询是否已存在
        if (!existing.empty()) { RESP_ERR(cb, "商户订单号已存在，请勿重复提交"); return; } // 订单号已存在

        std::string orderId = makeOrderId(); // 生成系统订单号
        long long priceCent = (long long)std::round(priceVal * 100); // 转换为分
        std::string payQf = db.getSetting("payQf", "1"); // 获取是否启用金额浮动

        // ═══════════════════════════════════════════════════════════════
        // 锁定不冲突的金额（防止多个订单使用相同金额）
        // ═══════════════════════════════════════════════════════════════
        long long realCent = priceCent; // 实际金额（分）
        bool locked = false; // 是否锁定成功
        for (int i = 0; i < 10; ++i) { // 最多尝试 10 次
            bool ok = db.exec( // 尝试插入金额
                "INSERT OR IGNORE INTO tmp_price(price,oid) VALUES(?,?)",
                {std::to_string(realCent), orderId});
            if (ok) { // 如果插入成功
                // 检查实际是否插入（INSERT OR IGNORE 失败时返回 true 但行没动）
                auto chk = db.queryOne("SELECT oid FROM tmp_price WHERE price=?",
                                       {std::to_string(realCent)});
                if (!chk.empty() && chk["oid"] == orderId) { locked = true; break; } // 确认锁定成功
            }
            if (payQf == "1") { // 如果启用金额浮动
                realCent = priceCent + (i % 2 == 0 ? 1 : -1) * (i / 2 + 1); // 浮动金额
                if (realCent <= 0) realCent = priceCent + i + 1; // 确保金额大于 0
            } else { // 否则不浮动
                break; // 退出循环
            }
        }
        if (!locked) { RESP_ERR(cb, "无法锁定金额，请稍后再试"); return; } // 锁定失败

        double reallyPrice = (double)realCent / 100.0; // 转换回元
        long long now = std::time(nullptr); // 获取当前时间
        int closeMin = 5; // 默认 5 分钟后关闭
        try { closeMin = std::stoi(db.getSetting("close", "5")); } catch (...) {} // 获取关闭时间
        long long closeDate = now + closeMin * 60; // 计算关闭时间

        std::string typeQrUrl = (type == "1") // 根据支付类型获取二维码
            ? db.getSetting("wxpay") : db.getSetting("zfbpay");

        // V 免签 type: 1=wxpay 2=alipay
        std::string payType = (type == "1") ? "wxpay" : "alipay"; // 转换支付类型
        bool ok = db.exec( // 插入订单到数据库
            "INSERT INTO pay_order(order_id,mch_order_no,param,pay_type,amount,real_amount,"
            "pay_url,notify_url,return_url,state,created_at,expire_time,pay_time)"
            " VALUES(?,?,?,?,?,?,?,?,?,0,?,?,0)",
            {orderId, payId, param, payType, fmtPrice(priceVal), fmtPrice(reallyPrice),
             typeQrUrl, notifyUrl, returnUrl,
             std::to_string(now), std::to_string(closeDate)});
        if (!ok) { // 如果插入失败
            db.exec("DELETE FROM tmp_price WHERE oid=?", {orderId}); // 删除锁定的金额
            RESP_ERR(cb, "创建订单失败"); return; // 返回错误
        }

        Json::Value data; // 创建响应数据
        data["orderId"]     = orderId; // 系统订单号
        data["payId"]       = payId; // 商户订单号
        data["price"]       = fmtPrice(priceVal); // 原始金额
        data["reallyPrice"] = fmtPrice(reallyPrice); // 实际金额
        data["payType"]     = std::stoi(type); // 支付类型
        data["payUrl"]      = typeQrUrl; // 二维码 URL
        data["expireTime"]  = (Json::Int64)closeDate; // 过期时间
        data["return_url"]  = returnUrl; // 返回 URL
        RESP_JSON(cb, AjaxResult::success("订单创建成功", data)); // 返回成功响应
    }

    // 检查订单状态方法
    void checkOrder(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string orderId) {
        auto &db = PayDb::instance(); // 获取数据库实例
        auto row = db.queryOne("SELECT state FROM pay_order WHERE order_id=?", {orderId}); // 查询订单状态
        Json::Value data; // 创建响应数据
        data["orderId"] = orderId; // 订单号
        data["state"]   = row.empty() ? -1 : std::stoi(row["state"]); // 订单状态（-1=不存在，0=待支付，1=已支付）
        RESP_OK(cb, data); // 返回成功响应
    }

    // 获取订单详细信息方法
    void getOrder(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                  std::string orderId) {
        auto &db = PayDb::instance(); // 获取数据库实例
        auto row = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId}); // 查询订单
        if (row.empty()) { RESP_ERR(cb, "订单不存在"); return; } // 订单不存在
        Json::Value data; // 创建响应数据
        data["orderId"]     = row["order_id"]; // 系统订单号
        data["payId"]       = row.count("mch_order_no") ? row.at("mch_order_no") : ""; // 商户订单号
        data["param"]       = row.count("param") ? row.at("param") : ""; // 额外参数
        // V 免签兼容: 返回 int payType (wxpay=1, alipay=2)
        std::string pt = row.count("pay_type") ? row.at("pay_type") : ""; // 获取支付类型
        data["payType"]     = (pt == "alipay" || pt == "2") ? 2 : 1; // 转换支付类型
        data["price"]       = std::stod(row.count("amount") ? row.at("amount") : "0"); // 原始金额
        data["reallyPrice"] = std::stod(row.count("real_amount") ? row.at("real_amount") : "0"); // 实际金额
        data["payUrl"]      = row.count("pay_url") ? row.at("pay_url") : ""; // 二维码 URL
        data["notify_url"]  = row.count("notify_url") ? row.at("notify_url") : ""; // 异步通知 URL
        data["return_url"]  = row.count("return_url") ? row.at("return_url") : ""; // 同步返回 URL
        data["state"]       = std::stoi(row["state"]); // 订单状态
        data["create_date"] = (Json::Int64)std::stoll(row.count("created_at") ? row.at("created_at") : "0"); // 创建时间
        data["close_date"]  = (Json::Int64)std::stoll(row.count("expire_time") ? row.at("expire_time") : "0"); // 过期时间
        data["pay_date"]    = (Json::Int64)std::stoll(row.count("pay_time") ? row.at("pay_time") : "0"); // 支付时间
        data["isAuto"]      = 1; // 是否自动监听
        RESP_OK(cb, data); // 返回成功响应
    }

    // 关闭过期订单方法
    void closeExpired(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int n = closeExpiredInternal(); // 关闭过期订单
        Json::Value data; data["closed"] = n; // 返回关闭数量
        RESP_OK(cb, data); // 返回成功响应
    }

private:
    // 内部关闭过期订单方法
    static int closeExpiredInternal() {
        auto &db = PayDb::instance(); // 获取数据库实例
        long long now = std::time(nullptr); // 获取当前时间
        auto rows = db.query( // 查询所有过期订单
            "SELECT order_id FROM pay_order WHERE state=0 AND expire_time>0 AND expire_time<?",
            {std::to_string(now)});
        int n = 0; // 关闭计数
        for (auto &r : rows) { // 遍历所有过期订单
            db.exec("UPDATE pay_order SET state=-1 WHERE order_id=?", {r["order_id"]}); // 标记为已关闭
            db.exec("DELETE FROM tmp_price WHERE oid=?", {r["order_id"]}); // 删除锁定的金额
            n++; // 计数加 1
        }
        return n; // 返回关闭数量
    }

    // 生成订单号方法
    static std::string makeOrderId() {
        auto now = std::time(nullptr); // 获取当前时间戳
        std::mt19937 rng((unsigned)std::random_device{}()); // 创建随机数生成器
        std::uniform_int_distribution<int> d(100000, 999999); // 生成 6 位随机数
        std::ostringstream oss; // 创建字符串流
        oss << now << d(rng); // 时间戳 + 随机数
        return oss.str(); // 返回订单号
    }
    // 格式化金额方法
    static std::string fmtPrice(double v) {
        std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; // 保留 2 位小数
        return oss.str(); // 返回格式化后的金额
    }
};
