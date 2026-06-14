#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <random> // 随机数库
#include <sstream> // 字符串流库
#include <iomanip> // 输入输出格式库
#include <cmath> // 数学库
#include <map> // 映射容器
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/EpaySign.h" // 易支付签名工具
#include "../common/HttpCaller.h" // HTTP 调用工具

// 易支付兼容接口 — 码支付 v1/v2 商户端
// POST/GET /submit.php  页面跳转支付
// POST     /mapi.php    API 支付 (JSON)
// GET      /api.php     查询订单 (act=order)
class EpayCtrl : public drogon::HttpController<EpayCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(EpayCtrl::submitGet,  "/submit.php", drogon::Get); // GET 支付页面路由
        ADD_METHOD_TO(EpayCtrl::submitPost, "/submit.php", drogon::Post); // POST 支付页面路由
        ADD_METHOD_TO(EpayCtrl::mapi,       "/mapi.php",   drogon::Post); // API 支付路由
        ADD_METHOD_TO(EpayCtrl::apiQuery,   "/api.php",    drogon::Get); // 查询订单路由
    METHOD_LIST_END // 路由列表结束

    // GET 支付页面处理
    void submitGet(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        handleSubmit(req, std::move(cb)); // 调用通用处理函数
    }
    // POST 支付页面处理
    void submitPost(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        handleSubmit(req, std::move(cb)); // 调用通用处理函数
    }

    // POST /mapi.php — API 支付接口
    void mapi(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req); // 提取请求参数
        std::string err, orderId, payUrl; // 定义输出参数
        double reallyPrice = 0; // 实际金额
        if (!createEpayOrder(params, err, orderId, reallyPrice, payUrl)) { // 创建订单
            Json::Value r; r["code"]=0; r["msg"]=err; // 创建失败响应
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return; // 返回错误
        }
        Json::Value r; // 创建成功响应
        r["code"]=1; r["msg"]="订单创建成功"; // 状态码和消息
        r["trade_no"]=orderId; // 系统订单号
        r["payurl"]="/pay/"+orderId; // 支付页面 URL
        r["pay_url"]=payUrl; // 二维码 URL
        r["really_price"]=reallyPrice; // V 免签兼容字段名
        cb(drogon::HttpResponse::newHttpJsonResponse(r)); // 返回成功响应
    }

    // GET /api.php?act=order&pid=&key=&out_trade_no=&sign= — 查询订单接口
    void apiQuery(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        if (req->getParameter("act") != "order") { // 检查 act 参数
            Json::Value r; r["code"]=0; r["msg"]="不支持的act"; // 不支持的操作
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return; // 返回错误
        }
        auto &db = PayDb::instance(); // 获取数据库实例
        std::string pid=req->getParameter("pid"); // 商户号
        std::string outTradeNo=req->getParameter("out_trade_no"); // 外部订单号
        std::string sign=req->getParameter("sign"); // 签名
        std::string sysKey=db.getSetting("key"); // 系统密钥
        std::string sysPid=db.getSetting("pid","1"); // 系统商户号
        if (pid!=sysPid) { // 检查商户号
            Json::Value r; r["code"]=0; r["msg"]="商户号错误"; // 商户号错误
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return; // 返回错误
        }
        std::map<std::string,std::string> vp; // 创建签名参数
        vp["act"]="order"; vp["key"]=sysKey; // 添加 act 和 key
        vp["out_trade_no"]=outTradeNo; vp["pid"]=pid; // 添加订单号和商户号
        if (EpaySign::sign(vp,sysKey)!=sign) { // 验证签名
            Json::Value r; r["code"]=0; r["msg"]="签名错误"; // 签名错误
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return; // 返回错误
        }
        auto order=db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=?",{outTradeNo}); // 查询订单
        if (order.empty()) { // 订单不存在
            Json::Value r; r["code"]=0; r["msg"]="订单不存在"; // 订单不存在
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return; // 返回错误
        }
        std::string typeStr=order.count("pay_type") ? order.at("pay_type") : ""; // 支付类型
        int state=std::stoi(order["state"]); // 订单状态
        Json::Value r; // 创建响应
        r["code"]=1; r["msg"]="查询成功"; // 成功状态
        r["trade_no"]=order["order_id"]; // 系统订单号
        r["out_trade_no"]=outTradeNo; // 外部订单号
        r["type"]=typeStr; // 支付类型
        r["name"]=order.count("subject") ? order.at("subject") : ""; // 商品名称
        r["money"]=order.count("amount") ? order.at("amount") : ""; // 金额
        r["status"]=(state==1 ? 1 : 0); // 订单状态（1=已支付，0=未支付）
        cb(drogon::HttpResponse::newHttpJsonResponse(r)); // 返回响应
    }

private:
    // 处理支付页面提交（GET 和 POST 通用）
    void handleSubmit(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req); // 提取请求参数
        std::string err, orderId, payUrl; // 定义输出参数
        double reallyPrice = 0; // 实际金额
        if (!createEpayOrder(params, err, orderId, reallyPrice, payUrl)) { // 创建订单
            auto resp = drogon::HttpResponse::newHttpResponse(); // 创建错误响应
            resp->setStatusCode(drogon::k400BadRequest); // 设置 400 状态码
            resp->setBody(err); // 设置错误消息
            cb(resp); return; // 返回错误
        }
        auto resp=drogon::HttpResponse::newRedirectionResponse("/pay/"+orderId); // 重定向到支付页面
        cb(resp); // 返回重定向响应
    }

    // 创建易支付订单
    bool createEpayOrder(const std::map<std::string,std::string> &params,
                         std::string &err, std::string &orderId,
                         double &reallyPrice, std::string &payUrl) {
        auto get=[&](const std::string &k)->std::string{ // 参数获取辅助函数
            auto it=params.find(k); return it==params.end()?"":it->second; // 从 map 中获取参数
        };
        auto &db=PayDb::instance(); // 获取数据库实例
        std::string sysPid=db.getSetting("pid","1"); // 系统商户号
        std::string sysKey=db.getSetting("key"); // 系统密钥
        if (sysKey.empty()) { err="系统未配置通讯密钥"; return false; } // 检查密钥

        std::string pid=get("pid"); // 商户号
        std::string type=get("type"); // 支付类型
        std::string outTradeNo=get("out_trade_no"); // 外部订单号
        std::string notifyUrl=get("notify_url"); // 异步通知 URL
        std::string returnUrl=get("return_url"); // 同步返回 URL
        std::string name=get("name"); // 商品名称
        std::string money=get("money"); // 金额
        std::string sign=get("sign"); // 签名

        if (pid.empty()||type.empty()||outTradeNo.empty()||money.empty()||sign.empty()) { // 检查必要参数
            err="参数不完整"; return false; // 参数不完整
        }
        if (pid!=sysPid) { err="商户号错误"; return false; } // 商户号错误
        if (!EpaySign::verify(params, sysKey, sign)) { err="签名错误"; return false; } // 验证签名

        int payType=EpaySign::typeFromStr(type); // 转换支付类型
        if (payType!=1 && payType!=2) { err="支付类型错误"; return false; } // 检查支付类型
        double priceVal=0; // 初始化金额
        try { priceVal=std::stod(money); } catch (...) { err="金额格式错误"; return false; } // 解析金额
        if (priceVal<=0) { err="金额错误"; return false; } // 检查金额

        auto existing=db.queryOne("SELECT id FROM pay_order WHERE mch_order_no=?",{outTradeNo}); // 查询订单是否存在
        if (!existing.empty()) { err="商户订单号已存在"; return false; } // 订单已存在

        orderId=makeOrderId(); // 生成系统订单号
        reallyPrice=priceVal; // 初始化实际金额
        std::string payQf=db.getSetting("payQf","1"); // 获取是否启用金额浮动
        if (payQf=="1") { // 如果启用金额浮动
            reallyPrice=lockUniquePrice(db, priceVal, payType); // 锁定唯一金额
        }

        auto qr=db.queryOne( // 查询可用收款码
            "SELECT * FROM pay_qrcode WHERE type=? AND state=0 AND (price=0 OR ABS(price-?)<0.001) ORDER BY price DESC,id ASC LIMIT 1",
            {std::to_string(payType), fmtPrice(reallyPrice)});
        if (qr.empty()) { err="暂无可用收款码"; return false; } // 没有可用收款码
        payUrl=qr.count("pay_url") ? qr["pay_url"] : ""; // 获取收款码 URL
        if (payUrl.empty()) { err="收款码地址为空"; return false; } // 收款码地址为空

        long long now=std::time(nullptr); // 获取当前时间
        int closeMin=5; // 默认 5 分钟后关闭
        try { closeMin=std::stoi(db.getSetting("close","5")); } catch (...) {} // 获取关闭时间
        long long closeDate=now + closeMin*60; // 计算关闭时间

        // V 免签 type int 转新 pay_type string
        std::string payTypeStr = (payType == 1) ? "wxpay" : "alipay"; // 转换支付类型字符串
        bool ok=db.exec( // 插入订单
            "INSERT INTO pay_order(order_id,mch_order_no,param,pay_type,amount,real_amount,pay_url,notify_url,return_url,subject,state,created_at,expire_time) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,0,?,?)",
            {orderId,outTradeNo,get("param"),payTypeStr,fmtPrice(priceVal),fmtPrice(reallyPrice),payUrl,notifyUrl,returnUrl,name,std::to_string(now),std::to_string(closeDate)});
        if (!ok) { err="订单创建失败"; return false; } // 插入失败
        return true; // 返回成功
    }

    // 生成订单号方法
    static std::string makeOrderId() {
        auto now=std::time(nullptr); // 获取当前时间戳
        std::mt19937 rng((unsigned)std::random_device{}()); // 创建随机数生成器
        std::uniform_int_distribution<int> dist(100000,999999); // 生成 6 位随机数
        std::ostringstream oss; // 创建字符串流
        oss << now << dist(rng); // 时间戳 + 随机数
        return oss.str(); // 返回订单号
    }

    // 格式化金额方法
    static std::string fmtPrice(double v) {
        std::ostringstream oss; // 创建字符串流
        oss << std::fixed << std::setprecision(2) << v; // 保留 2 位小数
        return oss.str(); // 返回格式化后的金额
    }

    // 锁定唯一金额方法（防止多个订单使用相同金额）
    static double lockUniquePrice(PayDb &db, double price, int type) {
        long long base=(long long)std::round(price*100); // 转换为分
        std::string pt = (type == 1) ? "wxpay" : "alipay"; // 支付类型
        // 有重复才 +0.01，无重复直接用原始金额
        auto exist0=db.queryOne( // 查询是否存在相同金额的订单
            "SELECT id FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) AND ABS(CAST(amount AS REAL)-?)<0.001",
            {pt, fmtPrice((double)base/100.0)});
        if (exist0.empty()) return price; // 没有重复，返回原始金额
        for (int i=1;i<=20;++i) { // 最多尝试 20 次
            double real=(base+i)/100.0; // 浮动金额（+0.01）
            bool exists=!db.queryOne( // 查询浮动后的金额是否存在
                "SELECT id FROM pay_order WHERE state=0 AND LOWER(pay_type)=LOWER(?) AND ABS(CAST(amount AS REAL)-?)<0.001",
                {pt, fmtPrice(real)}).empty();
            if (!exists) return real; // 找到未使用的金额，返回
        }
        return price; // 无法找到唯一金额，返回原始金额
    }
};
