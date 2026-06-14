// WePay V3 原生监控协议控制器
// V3 vs V2 核心区别:
//   1. 签名改为 sorted-query-string HMAC-SHA256（与V2 "\n" 拼接不同）
//   2. 心跳上报设备状态: battery / network / app_ver
//   3. 多商户绑定: 设备一次心跳绑定多个 pid，push 时按 pid 路由
//   4. /ocr 端点: 截图上传→ZXing识别→金额匹配→MinIO存凭证
//   5. WsBus 推送 topic="v3_dev_{device_id}" / "v3_mch_{mch_id}"
//
// 端点:
//   POST /api/wepay/v3/heart    心跳 + 设备状态 + 多商户绑定
//   POST /api/wepay/v3/push     收款推送（order_id精确+金额兜底）
//   POST /api/wepay/v3/pending  拉取待支付订单（WS断线时降级）
//   POST /api/wepay/v3/ocr      截图OCR上传识别
//   GET  /admin/api/wepay/v3/devices  管理端设备列表
//   POST /admin/api/wepay/v3/device/kick    踢下线
//   POST /admin/api/wepay/v3/device/delete  删除设备
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <drogon/MultiPart.h> // 多部分表单解析
#include <ctime> // 时间库
#include <map> // 映射容器
#include <mutex> // 互斥锁
#include <sstream> // 字符串流库
#include <algorithm> // 算法库
#include <unordered_set> // 哈希集合
#include <json/json.h> // JSON 库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/Md5Utils.h" // MD5 和签名工具
#include "../common/NotifyChannels.h" // 通知渠道（OSS 服务）
#include "../common/WsBus.h" // WebSocket 消息总线
#include "../channel/VmqPlugin.h" // V 免签插件
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

#ifdef WEPAY_HAS_ZXING // 如果编译时启用 ZXing 库
#  include <ZXing/ReadBarcode.h> // 二维码读取
#  include <ZXing/Barcode.h> // 条形码
#  include <ZXing/ImageView.h> // 图像视图
#  include <ZXing/BarcodeFormat.h> // 条形码格式
#  include "../common/stb_image.h" // 图像加载库
#endif

// WePay V3 监控端控制器类
class WepayV3MonitorCtrl : public drogon::HttpController<WepayV3MonitorCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(WepayV3MonitorCtrl::heart,   "/api/wepay/v3/heart",   drogon::Post); // 心跳路由
        ADD_METHOD_TO(WepayV3MonitorCtrl::push,    "/api/wepay/v3/push",    drogon::Post); // 推送路由
        ADD_METHOD_TO(WepayV3MonitorCtrl::pending, "/api/wepay/v3/pending", drogon::Post); // 待支付订单查询路由
        ADD_METHOD_TO(WepayV3MonitorCtrl::ocr,     "/api/wepay/v3/ocr",    drogon::Post); // OCR 识别路由
        ADD_METHOD_TO(WepayV3MonitorCtrl::devices,      "/admin/api/wepay/v3/devices",        drogon::Get,  "AdminAuthFilter"); // 设备列表路由（需管理员认证）
        ADD_METHOD_TO(WepayV3MonitorCtrl::kickDevice,   "/admin/api/wepay/v3/device/kick",    drogon::Post, "AdminAuthFilter"); // 踢下线路由（需管理员认证）
        ADD_METHOD_TO(WepayV3MonitorCtrl::deleteDevice, "/admin/api/wepay/v3/device/delete",  drogon::Post, "AdminAuthFilter"); // 删除设备路由（需管理员认证）
    METHOD_LIST_END // 路由列表结束

    // ══════════════════════════════════════════════════════════════════
    // POST /api/wepay/v3/heart — 设备心跳（上报在线状态和设备信息）
    // ══════════════════════════════════════════════════════════════════
    // 请求体: { device_id, timestamp, nonce, sign,
    //          device_name?, device_model?, battery?, network?, app_ver?,
    //          pids?: ["mch_no1","mch_no2"]  }
    // 签名: HMAC-SHA256(secret, "device_id=x&nonce=x&timestamp=x&key=secret")
    void heart(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { resp(cb, 400, "请求体必须是 JSON"); return; } // 检查请求体
        auto &j = *body; // 获取 JSON 对象引用

        std::string deviceId  = j.get("device_id",  "").asString(); // 设备 ID
        std::string timestamp = j.get("timestamp",  "").asString(); // 时间戳
        std::string nonce     = j.get("nonce",       "").asString(); // 随机数
        std::string sign      = j.get("sign",        "").asString(); // 签名

        if (deviceId.empty() || timestamp.empty() || nonce.empty() || sign.empty()) { // 检查必要参数
            resp(cb, 400, "缺少必要参数 (device_id, timestamp, nonce, sign)"); return; // 参数不完整
        }

        // 1) 时间戳防重放（防止重放攻击）
        if (!checkTimestamp(timestamp)) { resp(cb, 403, "时间戳过期，请检查设备时间"); return; } // 时间戳过期
        // 2) Nonce 去重（防止重复请求）
        if (!consumeNonce(nonce)) { resp(cb, 403, "nonce 重复，疑似重放攻击"); return; } // Nonce 已使用

        // 3) 解析 pids（多商户绑定）
        std::vector<std::string> pids; // 商户号列表
        if (j.isMember("pids") && j["pids"].isArray()) { // 如果有 pids 数组
            for (auto &p : j["pids"]) pids.push_back(p.asString()); // 添加每个商户号
        } else if (j.isMember("pid") && !j["pid"].asString().empty()) { // 或者单个 pid
            pids.push_back(j["pid"].asString()); // 添加商户号
        }

        // 4) 取密钥: 平台 key 或首个商户 key
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string key = resolveKey(db, pids.empty() ? "" : pids[0]); // 解析密钥
        if (key.empty()) { resp(cb, 403, "无效密钥"); return; } // 密钥为空

        // 5) 验签（HMAC-SHA256）
        std::string expected = v3Sign({{"device_id", deviceId}, {"nonce", nonce}, {"timestamp", timestamp}}, key); // 计算期望签名
        if (sign != expected) { resp(cb, 403, "签名错误"); return; } // 签名不匹配

        // 6) 注册/更新设备
        std::string ip          = req->getPeerAddr().toIp(); // 获取请求 IP
        std::string deviceName  = j.get("device_name",  "").asString(); // 设备名称
        std::string deviceModel = j.get("device_model", "").asString(); // 设备型号
        int    battery  = j.get("battery",  -1).asInt(); // 电池电量
        std::string network = j.get("network",  "").asString(); // 网络状态
        std::string appVer  = j.get("app_ver",  "").asString(); // 应用版本
        long long now = std::time(nullptr); // 获取当前时间

        auto existing = db.queryOne("SELECT id,state FROM monitor_device WHERE device_id=?", {deviceId}); // 查询设备是否存在
        if (existing.empty()) { // 设备不存在
            db.exec( // 插入新设备
                "INSERT INTO monitor_device(device_id,device_name,device_model,ip,state,"
                "battery,network,app_ver,last_heart,created_at) VALUES(?,?,?,?,1,?,?,?,?,?)",
                {deviceId, deviceName, deviceModel, ip,
                 std::to_string(battery), network, appVer,
                 std::to_string(now), std::to_string(now)});
        } else { // 设备已存在
            if (existing["state"] == "0") { resp(cb, 403, "设备已被管理员禁用"); return; } // 设备被禁用
            db.exec( // 更新设备信息
                "UPDATE monitor_device SET last_heart=?,ip=?,device_name=?,device_model=?,"
                "battery=?,network=?,app_ver=? WHERE device_id=?",
                {std::to_string(now), ip, deviceName, deviceModel,
                 std::to_string(battery), network, appVer, deviceId});
        }

        // 7) 更新多商户绑定（设备可以绑定多个商户）
        for (auto &pid : pids) { // 遍历每个商户号
            auto m = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {pid}); // 查询商户
            if (m.empty()) continue; // 商户不存在则跳过
            db.exec( // 插入或更新绑定关系
                "INSERT OR REPLACE INTO monitor_device_merchant(device_id,mch_id,last_heart) VALUES(?,?,?)",
                {deviceId, m["id"], std::to_string(now)});
        }

        // 8) 更新全局 V3 心跳状态（用于监控设备在线状态）
        db.setSetting("wepay_v3_lastheart", std::to_string(now)); // 记录最后心跳时间
        db.setSetting("wepay_v3_jkstate",   "1"); // 标记为在线

        // 9) WsBus 广播心跳事件（通知前端设备状态）
        Json::Value ev; // 创建事件对象
        ev["device_id"]    = deviceId; // 设备 ID
        ev["device_name"]  = deviceName; // 设备名称
        ev["battery"]      = battery; // 电池电量
        ev["network"]      = network; // 网络状态
        ev["app_ver"]      = appVer; // 应用版本
        ev["ip"]           = ip; // IP 地址
        ev["protocol"]     = "wepay_v3"; // 协议标记
        WsBus::instance().publishLive("v3_heartbeat", ev); // 发布心跳事件

        Json::Value r; // 创建响应
        r["code"] = 200; r["msg"] = "成功"; // 状态码和消息
        r["server_time"] = (Json::Int64)now; // 服务器时间
        cb(drogon::HttpResponse::newHttpJsonResponse(r)); // 返回响应
    }

    // ══════════════════════════════════════════════════════════════════
    // POST /api/wepay/v3/push — 收款推送（监控端上报收款信息）
    // ══════════════════════════════════════════════════════════════════
    // 请求体: { device_id, timestamp, nonce, sign,
    //          type, price, order_id?, pid? }
    // 签名: HMAC-SHA256(secret,
    //    "device_id=x&nonce=x&order_id=x&price=x&timestamp=x&type=x&key=secret")
    void push(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { resp(cb, 400, "请求体必须是 JSON"); return; } // 检查请求体
        auto &j = *body; // 获取 JSON 对象引用

        std::string deviceId  = j.get("device_id",  "").asString(); // 设备 ID
        std::string timestamp = j.get("timestamp",  "").asString(); // 时间戳
        std::string nonce     = j.get("nonce",       "").asString(); // 随机数
        std::string sign      = j.get("sign",        "").asString(); // 签名
        std::string type      = j.get("type",        "").asString(); // 支付类型
        std::string price     = j.get("price",       "").asString(); // 金额
        std::string orderId   = j.get("order_id",    "").asString(); // 订单号（可选）
        std::string pid       = j.get("pid",         "").asString(); // 商户号（可选）

        if (deviceId.empty() || timestamp.empty() || nonce.empty() || // 检查必要参数
            sign.empty() || type.empty() || price.empty()) {
            resp(cb, 400, "缺少必要参数"); return; // 参数不完整
        }

        if (!checkTimestamp(timestamp)) { resp(cb, 403, "时间戳过期"); return; } // 时间戳过期
        if (!consumeNonce(nonce))       { resp(cb, 403, "nonce 重复"); return; } // Nonce 已使用

        auto &db = PayDb::instance();
        std::string key;
        std::string mchId;

        // 优先用 pid 商户密钥
        if (!pid.empty()) {
            auto m = db.queryOne("SELECT id,vmq_key FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (m.empty()) { resp(cb, 403, "商户不存在或已禁用"); return; }
            key = m["vmq_key"]; mchId = m["id"];
            if (key.empty()) { resp(cb, 403, "商户未配置 vmq_key"); return; }
        } else {
            key = db.getSetting("key");
            // 尝试从设备绑定关系推断商户
            auto bind = db.queryOne(
                "SELECT mch_id FROM monitor_device_merchant WHERE device_id=? LIMIT 1", {deviceId});
            if (!bind.empty()) mchId = bind["mch_id"];
        }

        // 必须能确定商户身份，否则拒绝（防止无 pid 的设备跨商户核销订单）
        if (mchId.empty()) { resp(cb, 403, "无法确认商户身份，请在推送时携带 pid 参数"); return; }

        // 验签（包含 order_id）
        std::map<std::string,std::string> params = {
            {"device_id", deviceId}, {"nonce", nonce},
            {"price", price},        {"timestamp", timestamp}, {"type", type}
        };
        if (!orderId.empty()) params["order_id"] = orderId;
        std::string expected = v3Sign(params, key);
        if (sign != expected) { resp(cb, 403, "签名错误"); return; }

        // 设备校验
        auto dev = db.queryOne("SELECT id,state FROM monitor_device WHERE device_id=?", {deviceId});
        if (dev.empty()) { resp(cb, 403, "设备未注册，请先发送心跳"); return; }
        if (dev["state"] == "0") { resp(cb, 403, "设备已被禁用"); return; }

        // 订单匹配（强制带 mch_id 过滤，确保订单归属正确）
        long long now = std::time(nullptr);
        std::string priceFmt = fmtPrice(price);
        std::string payType  = typeToPayType(type);
        PayDb::Row order;

        if (!orderId.empty()) {
            order = db.queryOne(
                "SELECT * FROM pay_order WHERE order_id=? AND state=0 AND pay_type=? AND mch_id=?",
                {orderId, payType, mchId});
        }
        if (order.empty()) {
            order = db.queryOne(
                "SELECT * FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) AND mch_id=? "
                "AND (ABS(CAST(real_amount AS REAL)-?)<0.001 OR ABS(CAST(amount AS REAL)-?)<0.001) ORDER BY created_at ASC LIMIT 1",
                {payType, mchId, priceFmt, priceFmt});
        }

        // 更新设备推送统计
        db.exec("UPDATE monitor_device SET last_push=?,push_count=push_count+1 WHERE device_id=?",
                {std::to_string(now), deviceId});
        db.setSetting("wepay_v3_lastpay", std::to_string(now));

        if (order.empty()) {
            Json::Value r; r["code"] = 200; r["msg"] = "成功";
            r["matched"] = false; r["detail"] = "未匹配到待支付订单";
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }

        std::string matchedOrderId = order["order_id"];
        std::string matchedAmount  = order.count("real_amount") ? order["real_amount"] : priceFmt;

        // 标记已付 + 触发回调（复用 VmqPlugin 统一逻辑）
        std::string callKey = db.getSetting("key");
        if (!mchId.empty()) {
            auto m = db.queryOne("SELECT mch_key FROM merchant WHERE id=?", {mchId});
            if (!m.empty() && !m["mch_key"].empty()) callKey = m["mch_key"];
        }
        VmqPlugin::markOrderPaidStatic(order, payType, callKey);

        // WsBus 推送: 通知前端 + 通知设备
        Json::Value pev;
        pev["device_id"] = deviceId; pev["type"] = type;
        pev["price"] = priceFmt; pev["order_id"] = matchedOrderId;
        pev["matched"] = true; pev["protocol"] = "wepay_v3";
        WsBus::instance().publishLive("v3_push", pev);
        // 推给特定设备的 WS 连接
        WsBus::instance().publish("v3_dev_" + deviceId, pev);

        Json::Value r;
        r["code"] = 200; r["msg"] = "成功";
        r["matched"] = true;
        r["order_id"] = matchedOrderId;
        r["amount"]   = matchedAmount;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════════════
    //  POST /api/wepay/v3/pending
    //  Body: { device_id, timestamp, nonce, sign, pid? }
    //  sign = HMAC-SHA256(secret, "device_id=x&nonce=x&timestamp=x&key=secret")
    //  返回设备绑定商户的待支付订单（WS断线时的降级方案）
    // ══════════════════════════════════════════════════════════════════
    void pending(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { resp(cb, 400, "请求体必须是 JSON"); return; }
        auto &j = *body;

        std::string deviceId  = j.get("device_id",  "").asString();
        std::string timestamp = j.get("timestamp",  "").asString();
        std::string nonce     = j.get("nonce",       "").asString();
        std::string sign      = j.get("sign",        "").asString();
        std::string pid       = j.get("pid",         "").asString();

        if (deviceId.empty() || timestamp.empty() || nonce.empty() || sign.empty()) {
            resp(cb, 400, "缺少必要参数"); return;
        }
        if (!checkTimestamp(timestamp)) { resp(cb, 403, "时间戳过期"); return; }
        if (!consumeNonce(nonce))       { resp(cb, 403, "nonce 重复"); return; }

        auto &db = PayDb::instance();
        std::string key = resolveKey(db, pid);
        if (key.empty()) { resp(cb, 403, "无效密钥"); return; }

        std::string expected = v3Sign({{"device_id", deviceId}, {"nonce", nonce}, {"timestamp", timestamp}}, key);
        if (sign != expected) { resp(cb, 403, "签名错误"); return; }

        // 按设备绑定商户查待支付订单
        std::vector<PayDb::Row> orders;
        if (!pid.empty()) {
            auto m = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (!m.empty()) {
                orders = db.query(
                    "SELECT order_id,pay_type,real_amount,created_at,expired_at "
                    "FROM pay_order WHERE state=0 AND mch_id=? "
                    "AND expired_at>? ORDER BY created_at ASC LIMIT 50",
                    {m["id"], std::to_string(std::time(nullptr))});
            }
        } else {
            // 取该设备绑定的所有商户订单
            auto binds = db.query("SELECT mch_id FROM monitor_device_merchant WHERE device_id=?", {deviceId});
            for (auto &b : binds) {
                auto rows = db.query(
                    "SELECT order_id,pay_type,real_amount,created_at,expired_at "
                    "FROM pay_order WHERE state=0 AND mch_id=? "
                    "AND expired_at>? ORDER BY created_at ASC LIMIT 20",
                    {b["mch_id"], std::to_string(std::time(nullptr))});
                orders.insert(orders.end(), rows.begin(), rows.end());
            }
        }

        Json::Value list(Json::arrayValue);
        for (auto &o : orders) {
            Json::Value item;
            item["order_id"]   = o["order_id"];
            item["pay_type"]   = o["pay_type"];
            item["amount"]     = o["real_amount"];
            item["created_at"] = o["created_at"];
            item["expired_at"] = o["expired_at"];
            list.append(item);
        }
        Json::Value r; r["code"] = 200; r["msg"] = "成功"; r["data"] = list;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════════════
    //  POST /api/wepay/v3/ocr  (multipart/form-data)
    //  Fields: device_id, timestamp, nonce, sign, type, pid?
    //  File:   screenshot (image/*)
    //  sign = HMAC-SHA256(secret, "device_id=x&nonce=x&timestamp=x&type=x&key=secret")
    //  处理: 识别金额 → 匹配订单 → 截图存 MinIO → 返回结果
    // ══════════════════════════════════════════════════════════════════
    void ocr(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0) { resp(cb, 400, "multipart 解析失败"); return; }

        auto fp = [&](const std::string &k) -> std::string {
            return parser.getParameters().count(k) ? parser.getParameters().at(k) : "";
        };
        std::string deviceId  = fp("device_id");
        std::string timestamp = fp("timestamp");
        std::string nonce     = fp("nonce");
        std::string sign      = fp("sign");
        std::string type      = fp("type");
        std::string pid       = fp("pid");

        if (deviceId.empty() || timestamp.empty() || nonce.empty() || sign.empty() || type.empty()) {
            resp(cb, 400, "缺少必要参数"); return;
        }
        if (!checkTimestamp(timestamp)) { resp(cb, 403, "时间戳过期"); return; }
        if (!consumeNonce(nonce))       { resp(cb, 403, "nonce 重复"); return; }

        auto &db = PayDb::instance();
        std::string key = resolveKey(db, pid);
        if (key.empty()) { resp(cb, 403, "无效密钥"); return; }

        std::string expected = v3Sign({
            {"device_id", deviceId}, {"nonce", nonce}, {"timestamp", timestamp}, {"type", type}
        }, key);
        if (sign != expected) { resp(cb, 403, "签名错误"); return; }

        // 设备校验
        auto dev = db.queryOne("SELECT id,state FROM monitor_device WHERE device_id=?", {deviceId});
        if (dev.empty()) { resp(cb, 403, "设备未注册"); return; }
        if (dev["state"] == "0") { resp(cb, 403, "设备已被禁用"); return; }

        if (parser.getFiles().empty()) { resp(cb, 400, "未上传截图文件"); return; }
        auto &file = parser.getFiles()[0];

        // 保存临时文件
        std::string tmpDir  = "./uploads/ocr_tmp";
        std::filesystem::create_directories(tmpDir);
        std::string fname   = std::to_string(std::time(nullptr)) + "_" + deviceId + ".png";
        std::string tmpPath = tmpDir + "/" + fname;
        file.saveAs(tmpPath);

        // OCR 识别金额
        std::string recognizedAmount;
#ifdef WEPAY_HAS_ZXING
        // 尝试 ZXing 二维码解析（获取付款链接中的金额参数）
        {
            int w, h, ch;
            auto *pixels = stbi_load(tmpPath.c_str(), &w, &h, &ch, 1);
            if (pixels) {
                ZXing::ImageView iv(pixels, w, h, ZXing::ImageFormat::Lum);
                ZXing::ReaderOptions opts;
                opts.setFormats(ZXing::BarcodeFormat::QRCode);
                opts.setTryHarder(true);
                auto results = ZXing::ReadBarcodes(iv, opts);
                for (auto &r : results) {
                    if (!r.isValid()) continue;
                    std::string text = r.text();
                    // 从付款URL提取金额，e.g. ?amount=12.34 或 ?money=12.34
                    for (auto &kw : {"amount=", "money=", "fee="}) {
                        auto pos = text.find(kw);
                        if (pos == std::string::npos) continue;
                        auto start = pos + strlen(kw);
                        auto end   = text.find_first_of("&?#", start);
                        recognizedAmount = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                        break;
                    }
                    if (!recognizedAmount.empty()) break;
                }
                stbi_image_free(pixels);
            }
        }
#endif

        if (recognizedAmount.empty()) {
            std::filesystem::remove(tmpPath);
            resp(cb, 422, "未能识别金额，请确保截图清晰"); return;
        }

        // 订单匹配
        std::string payType  = typeToPayType(type);
        std::string priceFmt = fmtPrice(recognizedAmount);
        std::string mchId;
        if (!pid.empty()) {
            auto m = db.queryOne("SELECT id FROM merchant WHERE mch_no=? AND state=1", {pid});
            if (!m.empty()) mchId = m["id"];
        }

        PayDb::Row order;
        if (!mchId.empty()) {
            order = db.queryOne(
                "SELECT * FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) AND mch_id=? "
                "AND (ABS(CAST(real_amount AS REAL)-?)<0.001 OR ABS(CAST(amount AS REAL)-?)<0.001) ORDER BY created_at ASC LIMIT 1",
                {payType, mchId, priceFmt, priceFmt});
        } else {
            order = db.queryOne(
                "SELECT * FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) "
                "AND (ABS(CAST(real_amount AS REAL)-?)<0.001 OR ABS(CAST(amount AS REAL)-?)<0.001) ORDER BY created_at ASC LIMIT 1",
                {payType, priceFmt, priceFmt});
        }

        if (order.empty()) {
            std::filesystem::remove(tmpPath);
            Json::Value r; r["code"] = 200; r["msg"] = "成功";
            r["matched"] = false; r["amount"] = recognizedAmount;
            r["detail"]  = "识别到金额但未匹配到待支付订单";
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }

        std::string matchedOrderId = order["order_id"];
        long long now = std::time(nullptr);

        // 截图存 MinIO（凭证留档）
        std::string ocrKey  = "ocr/" + std::to_string(now / 86400 * 86400) + "/" + matchedOrderId + ".png";
        std::string imgUrl  = OssService::instance().upload(ocrKey, tmpPath);
        if (imgUrl.empty()) imgUrl = "/uploads/ocr_tmp/" + fname;
        std::filesystem::remove(tmpPath);

        // 更新订单截图凭证
        db.exec("UPDATE pay_order SET screenshot_url=? WHERE order_id=?",
                {imgUrl, matchedOrderId});

        // 标记已付 + 触发回调
        VmqPlugin::markOrderPaidStatic(order, payType, db.getSetting("key"));

        // WsBus 通知
        Json::Value ev;
        ev["device_id"] = deviceId; ev["order_id"] = matchedOrderId;
        ev["amount"] = recognizedAmount; ev["screenshot"] = imgUrl;
        ev["protocol"] = "wepay_v3_ocr";
        WsBus::instance().publishLive("v3_ocr", ev);

        Json::Value r; r["code"] = 200; r["msg"] = "识别并匹配成功";
        r["matched"]    = true;
        r["order_id"]   = matchedOrderId;
        r["amount"]     = recognizedAmount;
        r["screenshot"] = imgUrl;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════════════
    // 管理端 — 设备列表（需管理员认证）
    // ══════════════════════════════════════════════════════════════════
    void devices(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto rows = PayDb::instance().query( // 查询所有设备及其绑定的商户
            "SELECT d.id, d.device_id, d.device_name, d.device_model, d.ip, d.state, "
            "d.battery, d.network, d.app_ver, d.last_heart, d.last_push, d.push_count, d.created_at, "
            "STRING_AGG(m.mch_no, ',') AS bound_mchs " // 聚合绑定的商户号
            "FROM monitor_device d "
            "LEFT JOIN monitor_device_merchant dm ON dm.device_id=d.device_id " // 关联绑定关系
            "LEFT JOIN merchant m ON m.id=dm.mch_id::bigint " // 关联商户信息
            "GROUP BY d.id, d.device_id, d.device_name, d.device_model, d.ip, d.state, "
            "d.battery, d.network, d.app_ver, d.last_heart, d.last_push, d.push_count, d.created_at "
            "ORDER BY d.last_heart DESC LIMIT 200", {}); // 按最后心跳时间排序，限制 200 条

        long long now = std::time(nullptr); // 获取当前时间
        Json::Value list(Json::arrayValue); // 创建设备列表
        for (auto &row : rows) { // 遍历每个设备
            Json::Value item; // 创建设备项
            for (auto &kv : row) item[kv.first] = kv.second; // 复制所有字段
            long long lh = 0; // 初始化最后心跳时间
            try { lh = std::stoll(row.count("last_heart") ? row.at("last_heart") : "0"); } catch (...) {} // 解析最后心跳时间
            item["online"] = (now - lh <= 180); // 判断设备是否在线（180 秒内有心跳）
            list.append(item); // 添加到列表
        }
        RESP_OK(cb, list); // 返回设备列表
    }

    // 踢下线设备（禁用设备）
    void kickDevice(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 检查请求体
        std::string did = (*body).get("device_id", "").asString(); // 获取设备 ID
        if (did.empty()) { RESP_ERR(cb, "device_id 必填"); return; } // 检查设备 ID
        PayDb::instance().exec("UPDATE monitor_device SET state=0 WHERE device_id=?", {did}); // 禁用设备
        RESP_MSG(cb, "已踢下线"); // 返回成功消息
    }

    // 删除设备（完全删除）
    void deleteDevice(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject(); // 解析 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 检查请求体
        std::string did = (*body).get("device_id", "").asString(); // 获取设备 ID
        if (did.empty()) { RESP_ERR(cb, "device_id 必填"); return; } // 检查设备 ID
        PayDb::instance().exec("DELETE FROM monitor_device_merchant WHERE device_id=?", {did}); // 删除绑定关系
        PayDb::instance().exec("DELETE FROM monitor_device WHERE device_id=?", {did}); // 删除设备
        RESP_MSG(cb, "已删除"); // 返回成功消息
    }

private:
    // ═══════════════════════════════════════════════════════════════════
    // 防重放 nonce 缓存（60 秒窗口，内存存储）
    // ═══════════════════════════════════════════════════════════════════
    static std::mutex nonceMu_; // 互斥锁（保护 nonce 集合）
    static std::unordered_set<std::string> usedNonces_; // 已使用的 nonce 集合

    // 检查时间戳是否有效（防止重放攻击）
    static bool checkTimestamp(const std::string &ts, int windowSec = 60) {
        try {
            long long t = std::stoll(ts); // 解析时间戳
            long long now = std::time(nullptr); // 获取当前时间
            return std::abs(now - t) <= windowSec; // 检查时间差是否在窗口内
        } catch (...) { return false; } // 解析失败则返回 false
    }

    // 消费 nonce（标记为已使用）
    static bool consumeNonce(const std::string &nonce) {
        std::lock_guard<std::mutex> lock(nonceMu_); // 加锁
        if (usedNonces_.count(nonce)) return false; // 如果已使用则返回 false
        usedNonces_.insert(nonce); // 添加到已使用集合
        if (usedNonces_.size() > 100000) usedNonces_.clear(); // 防止内存溢出，定期清空
        return true; // 返回成功
    }

    // ═══════════════════════════════════════════════════════════════════
    // V3 签名: sorted-query-string HMAC-SHA256
    // ═══════════════════════════════════════════════════════════════════
    // 参数按 key 字典序升序排列，拼成 k=v&...，末尾追加 key=secret
    static std::string v3Sign(std::map<std::string,std::string> params, const std::string &secret) {
        std::string msg; // 签名消息
        for (auto &kv : params) { // std::map 已按 key 排序
            if (!msg.empty()) msg += '&'; // 添加分隔符
            msg += kv.first + '=' + kv.second; // 添加参数对
        }
        msg += "&key=" + secret; // 末尾追加密钥
        return Md5Utils::hmacSha256(secret, msg); // 计算 HMAC-SHA256
    }

    // ═══════════════════════════════════════════════════════════════════
    // 解析密钥（优先使用商户密钥，否则使用平台密钥）
    // ═══════════════════════════════════════════════════════════════════
    static std::string resolveKey(PayDb &db, const std::string &pid) {
        if (!pid.empty()) { // 如果提供了商户号
            auto m = db.queryOne("SELECT vmq_key FROM merchant WHERE mch_no=? AND state=1", {pid}); // 查询商户密钥
            if (!m.empty() && !m["vmq_key"].empty()) return m["vmq_key"]; // 如果找到则返回商户密钥
        }
        return db.getSetting("key"); // 否则返回平台密钥
    }

    // ═══════════════════════════════════════════════════════════════════
    // 工具方法
    // ═══════════════════════════════════════════════════════════════════
    
    // 格式化金额（保留 2 位小数）
    static std::string fmtPrice(const std::string &price) {
        try {
            double d = std::stod(price); // 转换为浮点数
            std::ostringstream oss; // 创建字符串流
            oss << std::fixed << std::setprecision(2) << d; // 保留 2 位小数
            return oss.str(); // 返回格式化后的字符串
        } catch (...) { return price; } // 转换失败则返回原值
    }

    // 支付类型转换（数字或字符串转标准格式）
    static std::string typeToPayType(const std::string &type) {
        if (type == "2" || type == "alipay" || type == "ali") return "alipay"; // 支付宝
        if (type == "3" || type == "qqpay"  || type == "qq")  return "qqpay"; // QQ 钱包
        return "wxpay"; // 默认微信支付
    }

    // 返回响应（JSON 格式）
    static void resp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                     int code, const std::string &msg) {
        Json::Value r; r["code"] = code; r["msg"] = msg; // 创建响应
        auto res = drogon::HttpResponse::newHttpJsonResponse(r); // 创建 HTTP 响应
        res->setStatusCode(code == 200 ? drogon::k200OK : drogon::k403Forbidden); // 设置 HTTP 状态码
        cb(res); // 返回响应
    }
};

inline std::mutex WepayV3MonitorCtrl::nonceMu_;
inline std::unordered_set<std::string> WepayV3MonitorCtrl::usedNonces_;
