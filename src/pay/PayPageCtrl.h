// 支付页面控制器
// 用途：后端渲染 /pay/{orderId} 与 /pay/result/{orderId}（提供给易支付/前端跳转），
// 也可纯返回订单 JSON 给前端 /pay-page 的 Vue 组件使用。
// 此处提供最小可用版：返回订单数据 + 简易 HTML 占位（Vue SPA 接管时会通过 /getOrder API）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <sstream> // 字符串流库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作

// 支付页面控制器类
class PayPageCtrl : public drogon::HttpController<PayPageCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(PayPageCtrl::payPage,    "/pay/{orderId}",        drogon::Get); // 支付页面路由
        ADD_METHOD_TO(PayPageCtrl::resultPage, "/pay/result/{orderId}", drogon::Get); // 支付结果页面路由
    METHOD_LIST_END // 路由列表结束

    // 支付页面处理
    void payPage(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                 std::string orderId) {
        auto &db = PayDb::instance(); // 获取数据库实例
        auto row = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId}); // 查询订单
        if (row.empty()) { // 如果订单不存在
            auto resp = drogon::HttpResponse::newHttpResponse(); // 创建响应
            resp->setStatusCode(drogon::k404NotFound); // 设置 404 状态码
            resp->setBody("订单不存在"); // 设置响应体
            cb(resp); return; // 返回响应
        }

        // 如果配置了 Vue 前端 frontend_url，直接 302 跳转到前端支付页面
        std::string fe = drogon::app().getCustomConfig()["wepay"]
                            .get("frontend_url", "").asString(); // 获取前端 URL
        if (!fe.empty()) { // 如果前端 URL 存在
            auto resp = drogon::HttpResponse::newRedirectionResponse(
                fe + "/#/payment/" + orderId); // 创建重定向响应
            cb(resp); return; // 返回重定向
        }

        // 否则返回内联简易支付页（HTML 页面）
        std::ostringstream html; // 创建 HTML 字符串流
        html << "<!doctype html><html><head><meta charset=\"utf-8\">" // HTML 头部
             << "<title>支付</title>" // 页面标题
             << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" // 响应式视口
             << "</head><body style=\"font-family:sans-serif;text-align:center;padding:40px\">" // 页面样式
             << "<h2>" << (row.count("pay_type") && row.at("pay_type") == "alipay" ? "支付宝支付" : "微信支付") << "</h2>" // 支付方式标题
             << "<p>订单号：" << orderId << "</p>" // 显示订单号
             << "<p>金额：¥" << (row.count("real_amount") ? row.at("real_amount") : "0.00") << "</p>" // 显示金额
             << "<p>请扫码支付：</p>" // 提示扫码
             << "<p><a href=\"" << row["pay_url"] << "\">" << row["pay_url"] << "</a></p>" // 显示支付二维码链接
             << "<p id=\"status\">等待支付...</p>" // 支付状态提示
             << "<script>setInterval(()=>fetch('/checkOrder/" << orderId << "')" // 定时检查订单状态
             << ".then(r=>r.json()).then(j=>{if(j.data&&j.data.state===1)" // 如果订单已支付
             << "{document.getElementById('status').innerText='支付成功！';" // 更新状态为成功
             << "setTimeout(()=>location.href='/pay/result/" << orderId << "',1500);}}),3000);" // 1.5 秒后跳转到结果页
             << "</script></body></html>"; // 脚本结束
        auto resp = drogon::HttpResponse::newHttpResponse(); // 创建响应
        resp->setContentTypeCode(drogon::CT_TEXT_HTML); // 设置内容类型为 HTML
        resp->setBody(html.str()); // 设置响应体
        cb(resp); // 返回响应
    }

    // 支付结果页面处理
    void resultPage(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string orderId) {
        auto &db = PayDb::instance(); // 获取数据库实例
        auto row = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId}); // 查询订单
        if (row.empty()) { // 如果订单不存在
            auto resp = drogon::HttpResponse::newHttpResponse(); // 创建响应
            resp->setStatusCode(drogon::k404NotFound); // 设置 404 状态码
            resp->setBody("订单不存在"); cb(resp); return; // 返回响应
        }
        std::string returnUrl = row.count("return_url") ? row["return_url"] : ""; // 获取返回 URL
        if (row["state"] == "1" && !returnUrl.empty()) { // 如果订单已支付且有返回 URL
            auto resp = drogon::HttpResponse::newRedirectionResponse(returnUrl); // 创建重定向响应
            cb(resp); return; // 跳转到返回 URL
        }
        std::string fe = drogon::app().getCustomConfig()["wepay"]
                            .get("frontend_url", "").asString(); // 获取前端 URL
        if (!fe.empty()) { // 如果前端 URL 存在
            auto resp = drogon::HttpResponse::newRedirectionResponse(
                fe + "/#/payment/result/" + orderId); // 创建重定向响应
            cb(resp); return; // 跳转到前端结果页
        }
        auto resp = drogon::HttpResponse::newHttpResponse(); // 创建响应
        resp->setContentTypeCode(drogon::CT_TEXT_HTML); // 设置内容类型为 HTML
        resp->setBody("<h2>" + std::string(row["state"] == "1" ? "支付成功" : "未支付") // 显示支付状态
                      + "</h2><p>订单号：" + orderId + "</p>"); // 显示订单号
        cb(resp); // 返回响应
    }
};
