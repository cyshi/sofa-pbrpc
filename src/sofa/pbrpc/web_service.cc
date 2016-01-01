// Copyright (c) 2015 Baidu.com, Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: shichengyi@baidu.com (Shi Chengyi)

#include <sofa/pbrpc/web_service.h>
#include <sofa/pbrpc/http.h>
#include <sofa/pbrpc/http_rpc_request.h>
#include <sofa/pbrpc/rpc_server_impl.h>
#include <sofa/pbrpc/rpc_server_stream.h>

namespace sofa {
namespace pbrpc {

static std::string GetHostName(const std::string& ip)
{
    struct sockaddr_in addr;
    struct hostent *host;
    if (inet_aton(ip.c_str(), &addr.sin_addr) != 0)
    {
        host = gethostbyaddr((char*)&addr.sin_addr, 4, AF_INET); 
    }
    else
    {
        host = gethostbyname(ip.c_str());
    }
    if (host != NULL)
    {
        return host->h_name;
    }
    else
    {
        return ip;
    }
}

template <class T>
static std::string format_number(T num)
{
    std::ostringstream oss;
    oss << num;
    std::string str = oss.str();
    size_t len = str.size();
    std::string ret;
    for (size_t i = 0; i < str.size(); ++i)
    {
        if (i != 0 && len % 3 == 0)
        {
            ret.push_back(',');
        }
        ret.push_back(str.at(i));
        --len;
    }
    return ret;
}

WebService::WebService(const ServicePoolWPtr& service_pool)
    : _service_pool(service_pool)
    , _servlet_map_lock()
    , _servlet_map(new ServletMap())
    , _default_home(NULL)
    , _default_options(NULL)
    , _default_status(NULL)
    , _default_services(NULL)
    , _default_service(NULL)
{ }

WebService::~WebService()
{
    delete _default_home;
    _default_home = NULL;

    delete _default_options;
    _default_options = NULL;

    delete _default_status;
    _default_status = NULL;

    delete _default_services;
    _default_services = NULL;

    delete _default_service;
    _default_service = NULL;

    delete _default_ajaxapi;
    _default_ajaxapi = NULL;
}

void WebService::Init()
{
    _default_home = 
        sofa::pbrpc::NewPermanentExtClosure(this, &WebService::DefaultHome);

    RegisterServlet("/", _default_home);
    RegisterServlet("/home", _default_home);

    _default_options = 
        sofa::pbrpc::NewPermanentExtClosure(this, &WebService::DefaultOptions);
    RegisterServlet("/options", _default_options);

    _default_status = 
        sofa::pbrpc::NewPermanentExtClosure(this, &WebService::DefaultStatus);
    RegisterServlet("/status", _default_status);

    _default_services = 
        sofa::pbrpc::NewPermanentExtClosure(this, &WebService::DefaultServices);
    RegisterServlet("/services", _default_services);

    _default_service = 
        sofa::pbrpc::NewPermanentExtClosure(this, &WebService::DefaultService);
    RegisterServlet("/service", _default_service);

    _default_ajaxapi = 
        sofa::pbrpc::NewPermanentExtClosure(this, &WebService::DefaultAjaxApi);
    RegisterServlet("/ajaxapi", _default_ajaxapi);
}

void WebService::RegisterServlet(const std::string& path, Servlet servlet)
{
    std::string real_path(path);
    FormatPath(real_path);
    ScopedLocker<FastLock> _(_servlet_map_lock);
    if (!_servlet_map.unique())
    {
        ServletMapPtr servlet_map(new ServletMap(*_servlet_map));
        _servlet_map.swap(servlet_map);
    }
    (*_servlet_map)[real_path] = servlet;
}

bool WebService::RoutePage(
    const RpcRequestPtr& rpc_request,
    const RpcServerStreamWPtr& server_stream)
{
    const HTTPRpcRequest* http_rpc_request = 
        static_cast<const HTTPRpcRequest*>(rpc_request.get());

    HTTPRequest request;
    request.path = http_rpc_request->_path;
    request.body = http_rpc_request->_req_body->ToString();
    request.headers = http_rpc_request->_headers;
    request.query_params = http_rpc_request->_query_params;
    request.decoded_path = http_rpc_request->_decoded_path;

    HTTPResponse response;
    response.ip = HostOfRpcEndpoint(http_rpc_request->_local_endpoint);
    response.port = PortOfRpcEndpoint(http_rpc_request->_local_endpoint);
    response.host = GetHostName(response.ip);

    bool ret = false;
    const std::string& method = http_rpc_request->_method;
    std::string real_path(method);
    FormatPath(real_path);
    Servlet servlet = FindServlet(real_path);
    if (servlet)
    {
        ret = servlet->Run(request, response);
        if (ret)
        {
            const_cast<HTTPRpcRequest*>(http_rpc_request)->SendResponse(
                server_stream, response);
        }
    }
    return ret;
}

Servlet WebService::FindServlet(const std::string& path)
{
    // path => /xxxx/ => ["", "xxx", ""]
    std::vector<std::string> path_vec;
    StringUtils::split(path, "/", &path_vec);
    std::size_t path_len = path.size();
    ServletMap::iterator map_it;
    std::vector<std::string>::reverse_iterator vec_it = path_vec.rbegin();
    ServletMapPtr servlets = GetServletPtr();
    size_t sub_len = 0;
    for (; vec_it != path_vec.rend(); ++vec_it)
    {
        const std::string& subpath = path.substr(0, path_len - sub_len);
        map_it = servlets->find(subpath);
        if (map_it != servlets->end())
        {
            return map_it->second;
        }
        sub_len += vec_it->size() + 1;
    }
    return map_it == servlets->end() ? NULL : map_it->second;
}

bool WebService::DefaultHome(const HTTPRequest& /*request*/,
                             HTTPResponse& response)
{
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    std::ostringstream oss;
    PageHeader(oss);
    ServerBrief(oss, service_pool, response);
    ServerStatus(oss, service_pool);
    ServiceList(oss, service_pool);
    ServerOptions(oss, service_pool);
    ListServlet(oss);
    PageFooter(oss);
    response.content = oss.str();
    return true;
}

bool WebService::DefaultOptions(const HTTPRequest& /*request*/, 
                                HTTPResponse& response)
{
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    std::ostringstream oss;
    PageHeader(oss);
    oss << "<a href=\"/\">&lt;&lt;&lt;&lt;back to Home</a><br>";
    ServerOptions(oss, service_pool);
    PageFooter(oss);
    response.content = oss.str();
    return true;
}

bool WebService::DefaultStatus(const HTTPRequest& /*request*/, 
                               HTTPResponse& response)
{
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    std::ostringstream oss;
    PageHeader(oss);
    oss << "<a href=\"/\">&lt;&lt;&lt;&lt;back to Home</a><br>";
    ServerStatus(oss, service_pool);
    PageFooter(oss);
    response.content = oss.str();
    return true;
}

bool WebService::DefaultServices(const HTTPRequest& /*request*/, 
                                 HTTPResponse& response)
{
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    std::ostringstream oss;
    PageHeader(oss);
    oss << "<a href=\"/\">&lt;&lt;&lt;&lt;back to Home</a><br>";
    ServiceList(oss, service_pool);
    PageFooter(oss);
    response.content = oss.str();
    return true;
}

bool WebService::DefaultService(const HTTPRequest& request, 
                                HTTPResponse& response)
{
    std::ostringstream oss;
    QueryParams::const_iterator it = request.query_params.find("name"); 
    if (it == request.query_params.end())
    {
        ErrorPage(oss, "Lack of name param");
        response.content = oss.str();
        return true;
    }
    const std::string& name = it->second;
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    ServiceBoard* svc_board = service_pool->FindService(name);
    if (svc_board == NULL)
    {
        ErrorPage(oss, "Service not found");
        response.content = oss.str();
        return true;
    }
    PageHeader(oss);
    oss << "<a href=\"/\">&lt;&lt;&lt;&lt;back to Home</a><br>"
        << "<a href=\"/services\">&lt;&lt;&lt;&lt;back to Services</a>";
    MethodList(oss, svc_board);
    PaintMethod(oss, svc_board);
    PageFooter(oss);
    response.content = oss.str();
    return true;
}

bool WebService::DefaultAjaxApi(const HTTPRequest& request, HTTPResponse& response)
{
    QueryParams::const_iterator it = request.query_params.find("api");
    if (it == request.query_params.end())
    {
        return false;
    }

    std::stringstream ss;
    ss << it->second;
    int api = -1;
    ss >> api;
    if (api == TABLE)
    {
        return GetTableData(request, response);
    }
    else if (api == CHART)
    {
        return GetChartData(request, response);
    }
    else
    {
        return false;
    }
}

void WebService::PageHeader(std::ostream& out)
{
    out << "<html>"
        << "<head><title>SOFA-PBRPC</title></head>"
        << "<body>";
}

void WebService::PageFooter(std::ostream& out)
{
    out << "<hr>"
        << "Provided by SOFA-PBRPC."
        << "</body>"
        << "</html>";
}

void WebService::ServerBrief(std::ostream& out,
                             const ServicePoolPtr& service_pool, 
                             const HTTPResponse& response)
{
    out << "<h1>" << response.host << "</h1>"
        << "<b>IP:</b> " << response.ip << "<br>"
        << "<b>Port:</b> " << response.port << "<br>"
        << "<b>Started:</b> " << ptime_to_string(service_pool->RpcServer()->GetStartTime()) << "<br>"
        << "<b>Version:</b> " << SOFA_PBRPC_VERSION << "<br>"
        << "<b>Compiled:</b> " << compile_info() << "<br>";
}

void WebService::ServerOptions(std::ostream& out,
                               const ServicePoolPtr& service_pool)
{
    RpcServerImpl* server = service_pool->RpcServer();
    RpcServerOptions options = server->GetOptions();
    out << "<h3>ServerOptions</h3><hr>"
        << "<table border=\"2\">"
        << "<tr><th align=\"left\">Name</th><th align=\"right\">Value</th></tr>"
        << "<tr><td>work_thread_num</td>"
        << "<td align=\"right\">" << options.work_thread_num << "</td></tr>"
        << "<tr><td>keep_alive_time (seconds)</td>"
        << "<td align=\"right\">" << options.keep_alive_time << "</td></tr>"
        << "<tr><td>max_pending_buffer_size (MB)</td>"
        << "<td align=\"right\">" << options.max_pending_buffer_size << "</td></tr>"
        << "<tr><td>max_throughput_in (MB)</td>"
        << "<td align=\"right\">" << options.max_throughput_in << "</td></tr>"
        << "<tr><td>max_throughput_out (MB)</td>"
        << "<td align=\"right\">" << options.max_throughput_out << "</td></tr>"
        << "<tr><td>disable_builtin_services</td>"
        << "<td align=\"right\">" << (options.disable_builtin_services ? "true" : "false") << "</td></tr>"
        << "<tr><td>disable_list_service</td>"
        << "<td align=\"right\">" << (options.disable_list_service ? "true" : "false") << "</td></tr>"
        << "</table>";
}

void WebService::ListServlet(std::ostream& out)
{
    ServletMapPtr servlets = GetServletPtr();
    ServletMap::iterator it = servlets->begin();
    out << "<h3>Servlets</h3><hr>" 
        << "<table border=\"2\">"
        << "<tr><th align=\"left\">Servlet</th></tr>";
    for (; it != servlets->end(); ++it)
    {
        const std::string& name = it->first;
        if (!it->first.empty())
        {
            out << "<tr><td><a href=\"" << name << "\">" << name << "</a></td></tr>";
        }
    }
    out << "</table>";
}

void WebService::ServerStatus(std::ostream& out,
                              const ServicePoolPtr& service_pool)
{
    RpcServerImpl* server = service_pool->RpcServer();
    int64 pending_message_count;
    int64 pending_buffer_size;
    int64 pending_data_size;
    server->GetPendingStat(
        &pending_message_count, &pending_buffer_size, &pending_data_size);
    out << "<h3>ServerStatus</h3><hr>"
        << "<table border=\"2\">"
        << "<tr><th align=\"left\">Name</td><th align=\"right\">Value</th></tr>"
        << "<tr><td>connection_count</td>"
        << "<td align=\"right\">" << server->ConnectionCount() << "</td></tr>"
        << "<tr><td>service_count</td>"
        << "<td align=\"right\">" << server->ServiceCount() << "</td></tr>"
        << "<tr><td>pending_message_count</td>"
        << "<td align=\"right\">" << pending_message_count << "</td></tr>"
        << "<tr><td>pending_buffer_size (bytes)</td>"
        << "<td align=\"right\">" << format_number(pending_buffer_size) << "</td></tr>"
        << "<tr><td>pending_data_size (bytes)</td>"
        << "<td align=\"right\">" << format_number(pending_data_size) << "</td></tr>"
        << "</table>";
}

void WebService::ServiceList(std::ostream& out,
                             const ServicePoolPtr& service_pool)
{
    out << "<h3>Services</h3><hr>"
        << "<table border=\"2\">"
        << "<tr>"
        << "<th rowspan=\"2\" align=\"left\">Name</th>"
        << "<th colspan=\"3\" align=\"center\">Stat in last second</th>"
        << "<th colspan=\"3\" align=\"center\">Stat in last minute</th>"
        << "</tr>"
        << "<tr>"
        << "<th align=\"right\">Requested</th><th align=\"right\">Succeed</th><th align=\"right\">Failed</th>"
        << "<th align=\"right\">Requested</th><th align=\"right\">Succeed</th><th align=\"right\">Failed</th>"
        << "</tr>";
    std::list<ServiceBoard*> svc_list;
    service_pool->ListService(&svc_list);
    for (std::list<ServiceBoard*>::iterator it = svc_list.begin();
            it != svc_list.end(); ++it)
    {
        ServiceBoard* svc_board = *it;
        std::string name = svc_board->ServiceName();
        sofa::pbrpc::builtin::ServiceStat stat1;
        sofa::pbrpc::builtin::ServiceStat stat60;
        svc_board->LatestStats(1, &stat1);
        svc_board->LatestStats(60, &stat60);
        out << "<tr>"
            << "<td><a href=\"/service?name=" << name << "\">" << name << "</a></td>"
            << "<td align=\"right\">" << (stat1.succeed_count() + stat1.failed_count()) << "</td>"
            << "<td align=\"right\">" << stat1.succeed_count() << "</td>" 
            << "<td align=\"right\">" << stat1.failed_count() << "</td>"
            << "<td align=\"right\">" << (stat60.succeed_count() + stat60.failed_count()) << "</td>"
            << "<td align=\"right\">" << stat60.succeed_count() << "</td>"
            << "<td align=\"right\">" << stat60.failed_count() << "</td>"
            << "</tr>";
    }
    out << "</table>";
}

void WebService::MethodList(std::ostream& out,
                            ServiceBoard* svc_board)
{
    out << "<h3>Methods of [" << svc_board->ServiceName() << "]</h3><hr>"
        << "<div id=\"detail\">" ;
    MethodDetail(out, svc_board);
    out << "</div>";
    out << "Notes: all the time in the table is in milliseconds.";
}

void WebService::MethodDetail(std::ostream& out, 
                              ServiceBoard* svc_board)
{
    out << "<table border=\"2\">"
        << "<tr>"
        << "<th rowspan=\"3\" align=\"left\">Name</th>"
        << "<th colspan=\"6\" align=\"center\">Stat in last second</th>"
        << "<th colspan=\"6\" align=\"center\">Stat in last minute</th>"
        << "</tr>"
        << "<tr>"
        << "<th colspan=\"3\" align=\"center\">Succeed</th>"
        << "<th colspan=\"3\" align=\"center\">Failed</th>"
        << "<th colspan=\"3\" align=\"center\">Succeed</th>"
        << "<th colspan=\"3\" align=\"center\">Failed</th>"
        << "</tr>"
        << "<tr>"
        << "<th align=\"right\">Count</th>"
        << "<th align=\"right\">AvgTime</th>"
        << "<th align=\"right\">MaxTime</th>"
        << "<th align=\"right\">Count</th>"
        << "<th align=\"right\">AvgTime</th>"
        << "<th align=\"right\">MaxTime</th>"
        << "<th align=\"right\">Count</th>"
        << "<th align=\"right\">AvgTime</th>"
        << "<th align=\"right\">MaxTime</th>"
        << "<th align=\"right\">Count</th>"
        << "<th align=\"right\">AvgTime</th>"
        << "<th align=\"right\">MaxTime</th>"
        << "</tr>";
    int method_count = svc_board->Descriptor()->method_count();
    for (int i = 0; i < method_count; ++i)
    {
        MethodBoard* method_board = svc_board->Method(i);
        std::string name = method_board->Descriptor()->name();
        sofa::pbrpc::builtin::MethodStat stat1;
        sofa::pbrpc::builtin::MethodStat stat60;
        method_board->LatestStats(1, &stat1);
        method_board->LatestStats(60, &stat60);
        out << "<tr>"
            << "<td>" << name << "</td>"
            << std::setprecision(6) << std::fixed
            << "<td align=\"right\">" << stat1.succeed_count() << "</td>"
            << "<td align=\"right\">" << stat1.succeed_avg_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << (float)stat1.succeed_max_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << stat1.failed_count() << "</td>"
            << "<td align=\"right\">" << stat1.failed_avg_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << (float)stat1.failed_max_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << stat60.succeed_count() << "</td>"
            << "<td align=\"right\">" << stat60.succeed_avg_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << (float)stat60.succeed_max_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << stat60.failed_count() << "</td>"
            << "<td align=\"right\">" << stat60.failed_avg_time_us() / 1000 << "</td>"
            << "<td align=\"right\">" << (float)stat60.failed_max_time_us() / 1000 << "</td>"
            << "</tr>";
    }
    out << "</table>";
}

void WebService::ErrorPage(std::ostream& out, 
                           const std::string& reason)
{
    PageHeader(out);
    out << "<h1>ERROR: " << reason << "</h1>";
    PageFooter(out);
}

// "dfs/" => "/dfs";
void WebService::FormatPath(std::string& path)
{
    path = (path[0] == '/') ? path : "/" + path;
    if (path[path.size() - 1] == '/')
    {
        path = path.substr(0, path.size() - 1);
    }
}

ServletMapPtr WebService::GetServletPtr()
{
    ScopedLocker<FastLock> _(_servlet_map_lock);
    return _servlet_map;
}

void WebService::PaintMethod(std::ostream& out, ServiceBoard* svc_board)
{
    int method_count = svc_board->Descriptor()->method_count();
    for (int i = 0; i < method_count; ++i)
    {
        out << "<hr><div id=\"main"<< i << "\" style=\"height:400px\"></div>";
    }
    out << "<script src=\"http://echarts.baidu.com/build/dist/echarts.js\"></script>";
    out << "<script src=\"http://apps.bdimg.com/libs/jquery/2.1.4/jquery.min.js\"></script>";
    out << "<script type=\"text/javascript\">";
    out << "require.config({";
    out << "paths: {";
    out << "echarts: 'http://echarts.baidu.com/build/dist'}});";

    for (int i = 0; i < method_count; ++i)
    {
        out << "var myChart" << i << ";";
        out << "function DrawChart" << i << "(ec)";
        out << "{myChart" << i << " = ec.init(document.getElementById('main" << i << "'));";
        out << "var option = {";
        MethodBoard* method_board = svc_board->Method(i);
        out << "title:{text:'"; 
        out << method_board->Descriptor()->full_name() << "'},";
        std::vector<StatSlot> stats;
        method_board->LatestStats(60, &stats);
        std::ostringstream oss;
        for (size_t j = 0; j < stats.size(); ++j)
        {
            oss << "\"" << j << "\","; 
        }
        out << "xAxis:[{type:'category', data:[" << oss.str() <<  "]}],";
        out << "yAxis:[{type:'value', data:[\"test\"]}],";
        oss.str("");
        std::vector<StatSlot>::reverse_iterator rit = stats.rbegin();
        for (; rit != stats.rend(); ++rit)
        {
            oss << rit->succeed_count << ",";
        }
        out << "series : [{\"name\":\"test\", \"type\":\"line\", \"data\":["
            << oss.str() << "]}]}; myChart" << i << ".setOption(option);}"; 
    }

    out << "function DrawCharts(ec) {";
    for (int i = 0; i < method_count; ++i)
    {
        out << "DrawChart" << i << "(ec);";
    }
    out << "}";
    out << "require(['echarts','echarts/chart/line'],DrawCharts);";
    out << "</script>";
    out << "<script type=\"text/javascript\">var timeTicket;";
    out << "clearInterval(timeTicket);timeTicket = setInterval(function(){";
    for (int i = 0; i < method_count; ++i)
    {
        out << "$.ajax({url:\"/ajaxapi?api=1&servicename=" << svc_board->ServiceName() 
            << "&methodid=" << i << "\", success: function(data){myChart"
            << i <<".addData([[0,data,false,false]])}, error: function(){myChart" 
            << i <<".addData([[0,0,false,false]])}});";
    }
    out << "$.ajax({url:\"/ajaxapi?api=0&servicename=" << svc_board->ServiceName() 
        << "\", success: function(data){ $('#detail').html(data); }});"
        << "}, 1000);";
    out << "</script>";
}

bool WebService::GetChartData(const HTTPRequest& request, HTTPResponse& response)
{
    std::ostringstream oss;
    QueryParams::const_iterator it = request.query_params.find("servicename"); 
    if (it == request.query_params.end())
    {
        return false;
    }
    const std::string& name = it->second;
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    ServiceBoard* svc_board = service_pool->FindService(name);
    if (svc_board == NULL)
    {
        return false;
    }
    const std::string& id = request.query_params.find("methodid")->second;
    std::stringstream ss;
    ss << id;
    int method_id = -1;
    ss >> method_id;
    int method_count = svc_board->Descriptor()->method_count();
    if (method_id < 0 || method_id >= method_count)
    {
        return false;
    }

    MethodBoard* method_board = svc_board->Method(method_id);
    std::vector<StatSlot> stats;
    method_board->LatestStats(1, &stats);
    ss.str("");
    ss.clear();
    ss << stats[0].succeed_count;
    response.content = ss.str();
    return true;
}

bool WebService::GetTableData(const HTTPRequest& request, HTTPResponse& response)
{
    std::ostringstream oss;
    QueryParams::const_iterator it = request.query_params.find("servicename"); 
    if (it == request.query_params.end())
    {
        return false;
    }
    const std::string& name = it->second;
    ServicePoolPtr service_pool = _service_pool.lock();
    if (!service_pool)
    {
        return false;
    }
    ServiceBoard* svc_board = service_pool->FindService(name);
    if (svc_board == NULL)
    {
        return false;
    }
    MethodDetail(oss, svc_board);
    response.content = oss.str();
    return true;
}

} // namespace pbrpc
} // namespace sofa
