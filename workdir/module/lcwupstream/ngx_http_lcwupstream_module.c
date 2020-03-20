//start from the very beginning,and to create greatness
//@author: Chuangwei Lin
//@E-mail：979951191@qq.com
//@brief： 使用upstream方式访问第三方服务

#include <ngx_config.h>//包含必要的头文件
#include <ngx_core.h>
#include <ngx_http.h>
//HTTP上下文结构体
typedef struct
{
    //ngx_http_status_t是HTTP框架提供的一个结构体
    //typedef struct{
    //ngx_uint_t code;
    //ngx_uint_t count;
    //u_char *start;
    //u_char *end;
    //}ngx_http_status_t
    ngx_http_status_t status;//上下文保存解析状态
    ngx_str_t backendServer;
} ngx_http_lcwupstream_ctx_t;
//配置结构体
typedef struct
{   //每个HTTP请求都会有独立的ngx_http_upstream_conf_t结构体，本例是
    //所有的请求都共享同一个结构体
    ngx_http_upstream_conf_t upstream;
} ngx_http_lcwupstream_conf_t;

//用于初始化hide_headers成员，作为ngx_http_upstream_hide_headers_hash函数的参数
static ngx_str_t  ngx_http_proxy_hide_headers[] =
{
    ngx_string("Date"),
    ngx_string("Server"),
    ngx_string("X-Pad"),
    ngx_string("X-Accel-Expires"),
    ngx_string("X-Accel-Redirect"),
    ngx_string("X-Accel-Limit-Rate"),
    ngx_string("X-Accel-Buffering"),
    ngx_string("X-Accel-Charset"),
    ngx_null_string
};

//先声明函数
static char *ngx_http_lcwupstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_lcwupstream_handler(ngx_http_request_t *r);
static void* ngx_http_lcwupstream_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_lcwupstream_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t lcw_upstream_process_header(ngx_http_request_t *r);
static ngx_int_t lcw_process_status_line(ngx_http_request_t *r);
static void lcw_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc);
static ngx_int_t lcw_upstream_create_request(ngx_http_request_t *r);
//ngx_command_t定义模块的配置文件参数
static ngx_command_t ngx_http_lcwupstream_commands[] =
{
    {
        //配置项名称
        ngx_string("lcwupstream"),
        //配置项类型，将指定配置项可以出现的位置
        //例如出现在server{}或location{}中，以及他可以携带的参数个数
         NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_NOARGS,
         //ngx_command_t结构体中的set成员，
         //当在某个配置块中出现lcwupstream配置项时，Nginx将会调用ngx_http_lcwupstream方法
         //ngx_http_lcwupstream方法将在下面实现
         ngx_http_lcwupstream,
         //在配置文件中的偏移量conf
         NGX_HTTP_LOC_CONF_OFFSET,
         //offset通常用于使用预设的解析方法解析配置项，需要与conf配合使用
         0,
         //配置项读取后的处理方法，必须是ngx_conf_post_t结构的指针
         NULL
    },
    //ngx_null_command是一个空的ngx_command_t结构，用来表示数组的结尾
    ngx_null_command
};
//ngx_http_module_t的8个回调方法，是必须在HTTP框架初始化时完成的
static ngx_http_module_t  ngx_http_lcwupstream_module_ctx =
{
    NULL,       // preconfiguration解析配置文件前调用
    NULL,       // postconfiguration 完成配置文件解析后调用

    NULL,       // create main configuration当需要创建数据结构用于存储main级别的
                //(直属于http{}块的配置项)的全局配置项时
    NULL,       // init main configuration常用于初始化main级别的配置项

    NULL,       // create server configuration当需要创建数据结构用于存储srv级别的
                //(直属于server{}块的配置项)的配置项时 
    NULL,       // merge server configuration用于合并main级别和srv级别下的同名配置项
    //这两个回调方法需要实现
    ngx_http_lcwupstream_create_loc_conf,  // create location configuration 当需要创建数据结构用于存储loc级别的
                                           //(直属于location{}块的配置项)的配置项时
    ngx_http_lcwupstream_merge_loc_conf    // merge location configuration 用于合并srv和loc级别下的同名配置项
};
/******************************************************
函数名：ngx_http_lcwupstream_create_loc_conf(ngx_conf_t *cf)
参数：
功能：ngx_http_lcwupstream_create_loc_conf回调方法
*******************************************************/
static void* ngx_http_lcwupstream_create_loc_conf(ngx_conf_t *cf)
{
    //配置结构体
    ngx_http_lcwupstream_conf_t  *mycf;
    mycf = (ngx_http_lcwupstream_conf_t  *)ngx_pcalloc(cf->pool, sizeof(ngx_http_lcwupstream_conf_t));
    if (mycf == NULL)
    {
        return NULL;
    }
    //以下简单的硬编码ngx_http_upstream_conf_t结构中的各成员，例如
    //超时时间都设为1分钟。这也是http反向代理模块的默认值
    mycf->upstream.connect_timeout = 60000;
    mycf->upstream.send_timeout = 60000;
    mycf->upstream.read_timeout = 60000;
    mycf->upstream.store_access = 0600;
    //实际上buffering已经决定了将以固定大小的内存作为缓冲区来转发上游的
    //响应包体，这块固定缓冲区的大小就是buffer_size。如果buffering为1
    //就会使用更多的内存缓存来不及发往下游的响应，例如最多使用bufs.num个
    //缓冲区、每个缓冲区大小为bufs.size，另外还会使用临时文件，临时文件的
    //最大长度为max_temp_file_size
    mycf->upstream.buffering = 0;
    mycf->upstream.bufs.num = 8;
    mycf->upstream.bufs.size = ngx_pagesize;
    mycf->upstream.buffer_size = ngx_pagesize;
    mycf->upstream.busy_buffers_size = 2 * ngx_pagesize;
    mycf->upstream.temp_file_write_size = 2 * ngx_pagesize;
    mycf->upstream.max_temp_file_size = 1024 * 1024 * 1024;
    //upstream模块要求hide_headers成员必须要初始化（upstream在解析
    //完上游服务器返回的包头时，会调用
    //ngx_http_upstream_process_headers方法按照hide_headers成员将
    //本应转发给下游的一些http头部隐藏），这里将它赋为
    //NGX_CONF_UNSET_PTR ，是为了在merge合并配置项方法中使用
    //upstream模块提供的ngx_http_upstream_hide_headers_hash
    //方法初始化hide_headers 成员
    mycf->upstream.hide_headers = NGX_CONF_UNSET_PTR;
    mycf->upstream.pass_headers = NGX_CONF_UNSET_PTR;
    return mycf;
}
/******************************************************
函数名：ngx_http_lcwupstream_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
参数：
功能：
*******************************************************/
static char *ngx_http_lcwupstream_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_lcwupstream_conf_t *prev = (ngx_http_lcwupstream_conf_t *)parent;
    ngx_http_lcwupstream_conf_t *conf = (ngx_http_lcwupstream_conf_t *)child;
    ngx_hash_init_t  hash;
    hash.max_size = 100;
    hash.bucket_size = 1024;
    hash.name = "proxy_headers_hash";
    //初始化hide_headers
    if (ngx_http_upstream_hide_headers_hash(cf, &conf->upstream, &prev->upstream, ngx_http_proxy_hide_headers, &hash)!= NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

//定义lcwupstream模块
//lcwupstream模块在编译时会被加入到ngx_modules全局数组中
//Nginx在启动时，会调用所有模块的初始化回调方法
//HTTP框架初始化时会调用ngx_http_module_t中的8个方法
//HTTP模块数据结构
ngx_module_t  ngx_http_lcwupstream_module =
{
    NGX_MODULE_V1,//该宏为下面的ctx_index,index，spare0，spare1，spare2，spare3，version变量
                  //提供了初始化的值：0,0,0,0,0,0,1
    //ctx_index表示当前模块在这类模块中的序号
    //index表示当前模块在所有模块中的序号，Nginx启动时会根据ngx_modules数组设置各模块的index值
    //spare0   spare系列的保留变量，暂未使用
    //spare1
    //spare2
    //spare3
    //version模块的版本，便于将来的扩展，目前只有一种，默认为1
    &ngx_http_lcwupstream_module_ctx, //ctx用于指向一类模块的上下文结构，8个回调方法
    ngx_http_lcwupstream_commands,   //commands将处理nginx.conf中的配置项
    NGX_HTTP_MODULE,        //模块的类型，与ctx指针紧密相关，取值范围是以下5种：
                            //NGX_HTTP_MODULE,NGX_CORE_MODULE,NGX_CONF_MODULE,NGX_EVENT_MODULE,NGX_MAIL_MODULE
    //以下7个函数指针表示有7个执行点会分别调用这7种方法，对于任一个方法而言，如果不需要nginx在某个是可执行它
    //那么简单地将他设为空指针即可
    NULL,                           //master进程启动时回调init_master
    NULL,                           //init_module回调方法在初始化所有模块时被调用，在master/worker模式下，
                                    //这个阶段将在启动worker子进程前完成
    NULL,                           //init_process回调方法在正常服务前被调用，在master/worker模式下，
                                    //多个worker子进程已经产生，在每个worker子进程的初始化过程会调用所有模块的init_process函数
    NULL,                           //由于nginx暂不支持多线程模式，所以init thread在框架代码中没有被调用过
    NULL,                           // exit thread,也不支持
    NULL,                           //exit process回调方法将在服务停止前调用，在master/worker模式下，worker进程会在退出前调用它
    NULL,                           //exit master回调方法将在master进程退出前被调用
    NGX_MODULE_V1_PADDING           //这里是8个spare_hook变量，是保留字段，目前没有使用，Nginx提供了NGX_MODULE_V1_PADDING宏来填充
};
/******************************************************
函数名：lcw_upstream_create_request(ngx_http_request_t *r)
参数：ngx_http_request_t结构
功能：
*******************************************************/
static ngx_int_t lcw_upstream_create_request(ngx_http_request_t *r)//
{
    //发往taobao上游服务器的请求很简单，就是模仿正常的搜索请求，
    //以/search?q=…的URL来发起搜索请求。backendQueryLine中的%V等转化
    //%V转换ngx_str_t
    //typedef struct{
    //size_t len;
    //u_char *data;
    //}ngx_str_t;
    //这里貌似要和搜索请求相对应，不是所有的搜索引擎请求都是search?q=
    static ngx_str_t backendQueryLine = ngx_string("GET /search?q=%V HTTP/1.1\r\nHost: s.taobao.com\r\nConnection: close\r\n\r\n");
    ngx_int_t queryLineLen = backendQueryLine.len + r->args.len - 2;
    //必须由内存池中申请内存，这有两点好处：在网络情况不佳的情况下，向上游
    //服务器发送请求时，可能需要epoll多次调度send发送才能完成，
    //这时必须保证这段内存不会被释放；请求结束时，这段内存会被自动释放，降低内存泄漏的可能
    ngx_buf_t* b = ngx_create_temp_buf(r->pool, queryLineLen);
    if (b == NULL)
        return NGX_ERROR;
    //last要指向请求的末尾
    b->last = b->pos + queryLineLen;

    //作用相当于snprintf
    ngx_snprintf(b->pos, queryLineLen ,
                 (char*)backendQueryLine.data, &r->args);
    // r->upstream->request_bufs是一个ngx_chain_t结构，它包含着要
    //发送给上游服务器的请求
    r->upstream->request_bufs = ngx_alloc_chain_link(r->pool);
    if (r->upstream->request_bufs == NULL)
        return NGX_ERROR;

    // request_bufs这里只包含1个ngx_buf_t缓冲区
    r->upstream->request_bufs->buf = b;
    r->upstream->request_bufs->next = NULL;

    r->upstream->request_sent = 0;
    r->upstream->header_sent = 0;
    // header_hash不可以为0
    r->header_hash = 1;
    return NGX_OK;
}
/******************************************************
函数名：lcw_process_status_line(ngx_http_request_t *r)
参数：ngx_http_request_t结构体
功能：解析上游服务器返回的基于TCP的响应头部，可能会被多次调用
      本例就是解析HTTP响应行和HTTP头部
*******************************************************/
static ngx_int_t lcw_process_status_line(ngx_http_request_t *r)
{
    size_t                 len;
    ngx_int_t              rc;
    ngx_http_upstream_t   *u;
    //上下文中才会保存多次解析http响应行的状态，首先取出请求的上下文
    ngx_http_lcwupstream_ctx_t* ctx = ngx_http_get_module_ctx(r, ngx_http_lcwupstream_module);
    if (ctx == NULL)
    {
        return NGX_ERROR;
    }

    u = r->upstream;
    //http框架提供的ngx_http_parse_status_line方法可以解析http
    //响应行，它的输入就是收到的字符流和上下文中的ngx_http_status_t结构
    rc = ngx_http_parse_status_line(r, &u->buffer, &ctx->status);
    //返回NGX_AGAIN表示还没有解析出完整的http响应行，需要接收更多的字符流再来解析
    if (rc == NGX_AGAIN)
    {
        return rc;
    }
    //返回NGX_ERROR则没有接收到合法的http响应行
    if (rc == NGX_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "upstream sent no valid HTTP/1.0 header");
        r->http_version = NGX_HTTP_VERSION_9;
        u->state->status = NGX_HTTP_OK;

        return NGX_OK;
    }
    //以下表示解析到完整的http响应行，这时会做一些简单的赋值操作，将解析出
    //的信息设置到r->upstream->headers_in结构体中，upstream解析完所
    //有的包头时，就会把headers_in中的成员设置到将要向下游发送的
    //r->headers_out结构体中，也就是说，现在我们向headers_in中设置的
    //信息，最终都会发往下游客户端。为什么不是直接设置r->headers_out而要
    //这样多此一举呢？这是因为upstream希望能够按照
    //ngx_http_upstream_conf_t配置结构体中的hide_headers等成员对
    //发往下游的响应头部做统一处理
    if (u->state)
    {
        u->state->status = ctx->status.code;
    }

    u->headers_in.status_n = ctx->status.code;

    len = ctx->status.end - ctx->status.start;
    u->headers_in.status_line.len = len;

    u->headers_in.status_line.data = ngx_pnalloc(r->pool, len);
    if (u->headers_in.status_line.data == NULL)
    {
        return NGX_ERROR;
    }

    ngx_memcpy(u->headers_in.status_line.data, ctx->status.start, len);

    //下一步将开始解析http头部，设置process_header回调方法为lcw_upstream_process_header
    //之后再收到的新字符流将由lcw_upstream_process_header解析
    u->process_header = lcw_upstream_process_header;
    //如果本次收到的字符流除了http响应行外，还有多余的字符，
    //将由lcw_upstream_process_header方法解析,lcw_upstream_process_header方法在下面实现
    return lcw_upstream_process_header(r);
}
/******************************************************
函数名：lcw_upstream_process_header(ngx_http_request_t *r)
参数：ngx_http_request_t结构体
功能：
*******************************************************/
static ngx_int_t lcw_upstream_process_header(ngx_http_request_t *r)
{
    ngx_int_t                       rc;
    ngx_table_elt_t                *h;
    ngx_http_upstream_header_t     *hh;
    ngx_http_upstream_main_conf_t  *umcf;
    //这里将upstream模块配置项ngx_http_upstream_main_conf_t取了
    //出来，目的只有1个，对将要转发给下游客户端的http响应头部作统一
    //处理。该结构体中存储了需要做统一处理的http头部名称和回调方法
    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);

    //循环的解析所有的http头部
    for ( ;; )
    {
        // http框架提供了基础性的ngx_http_parse_header_line方法，它用于解析http头部
        rc = ngx_http_parse_header_line(r, &r->upstream->buffer, 1);
        //返回NGX_OK表示解析出一行http头部
        if (rc == NGX_OK)
        {
            //向headers_in.headers这个ngx_list_t链表中添加http头部
            h = ngx_list_push(&r->upstream->headers_in.headers);
            if (h == NULL)
            {
                return NGX_ERROR;
            }
            //以下开始构造刚刚添加到headers链表中的http头部
            h->hash = r->header_hash;

            h->key.len = r->header_name_end - r->header_name_start;
            h->value.len = r->header_end - r->header_start;
            //必须由内存池中分配存放http头部的内存
            h->key.data = ngx_pnalloc(r->pool, h->key.len + 1 + h->value.len + 1 + h->key.len);
            if (h->key.data == NULL)
            {
                return NGX_ERROR;
            }

            h->value.data = h->key.data + h->key.len + 1;
            h->lowcase_key = h->key.data + h->key.len + 1 + h->value.len + 1;

            ngx_memcpy(h->key.data, r->header_name_start, h->key.len);
            h->key.data[h->key.len] = '\0';
            ngx_memcpy(h->value.data, r->header_start, h->value.len);
            h->value.data[h->value.len] = '\0';

            if (h->key.len == r->lowcase_index)
            {
                ngx_memcpy(h->lowcase_key, r->lowcase_header, h->key.len);
            }
            else
            {
                ngx_strlow(h->lowcase_key, h->key.data, h->key.len);
            }

            //upstream模块会对一些http头部做特殊处理
            hh = ngx_hash_find(&umcf->headers_in_hash, h->hash, h->lowcase_key, h->key.len);

            if (hh && hh->handler(r, h, hh->offset) != NGX_OK)
            {
                return NGX_ERROR;
            }

            continue;
        }
        //返回NGX_HTTP_PARSE_HEADER_DONE表示响应中所有的http头部都解析
        //完毕，接下来再接收到的都将是http包体
        if (rc == NGX_HTTP_PARSE_HEADER_DONE)
        {
            //如果之前解析http头部时没有发现server和date头部，以下会
            //根据http协议添加这两个头部
            if (r->upstream->headers_in.server == NULL)
            {   //没有发现server头部则添加该头部
                h = ngx_list_push(&r->upstream->headers_in.headers);
                if (h == NULL)
                {
                    return NGX_ERROR;
                }

                h->hash = ngx_hash(ngx_hash(ngx_hash(ngx_hash(ngx_hash('s', 'e'), 'r'), 'v'), 'e'), 'r');
                ngx_str_set(&h->key, "Server");
                ngx_str_null(&h->value);
                h->lowcase_key = (u_char *) "server";
            }
            if (r->upstream->headers_in.date == NULL)
            {   //没有发现date头部则添加date头部
                h = ngx_list_push(&r->upstream->headers_in.headers);
                if (h == NULL)
                {
                    return NGX_ERROR;
                }

                h->hash = ngx_hash(ngx_hash(ngx_hash('d', 'a'), 't'), 'e');
                ngx_str_set(&h->key, "Date");
                ngx_str_null(&h->value);
                h->lowcase_key = (u_char *) "date";
            }

            return NGX_OK;
        }

        //如果返回NGX_AGAIN则表示状态机还没有解析到完整的http头部，
        //要求upstream模块继续接收新的字符流再交由process_header回调方法解析
        if (rc == NGX_AGAIN)
        {
            return NGX_AGAIN;
        }
        //其他返回值都是非法的
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,"upstream sent invalid header");
        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }
}
/******************************************************
函数名：lcw_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
参数：
功能：释放资源
*******************************************************/
static void lcw_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    //在请求结束时，会调用该方法，可以释放资源，如打开的句柄等，由于我们没有任何需要释放的资源
    //故该方法没有任何实际的工作
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,"lcw_upstream_finalize_request");
}
/******************************************************
函数名：ngx_http_lcwupstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
参数：
功能：lcwupstream方法的实现
*******************************************************/
static char* ngx_http_lcwupstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;
    //首先找到lcwtest配置项所属的配置块，clcf貌似是location块内的数据
    //结构，其实不然，它可以是main、srv或者loc级别配置项，也就是说在每个
    //http{}和server{}内也都有一个ngx_http_core_loc_conf_t结构体
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    //http框架在处理用户请求进行到NGX_HTTP_CONTENT_PHASE阶段时，如果
    //请求的主机域名、URI与lcwupstream配置项所在的配置块相匹配，就将调用我们
    //实现的ngx_http_lcwupstream_handler方法处理这个请求
    //ngx_http_lcwupstream_handler将在下面实现
    clcf->handler = ngx_http_lcwupstream_handler;
    return NGX_CONF_OK;
}
/******************************************************
函数名：ngx_http_lcwupstream_handler(ngx_http_request_t *r)
参数：ngx_http_request_t结构体
功能：ngx_http_lcwupstream_handler方法的实现，启动upstream
*******************************************************/
static ngx_int_t ngx_http_lcwupstream_handler(ngx_http_request_t *r)
{
     //首先建立http上下文结构体ngx_http_lcwupstream_ctx_t
    //ngx_http_get_module_ctx是一个宏定义：(r)->ctx[module.ctx_index],r为ngx_http_request_t指针
    //第二个参数为HTTP模块对象
    ngx_http_lcwupstream_ctx_t* myctx = ngx_http_get_module_ctx(r, ngx_http_lcwupstream_module);
    if (myctx == NULL)//失败
    {//开辟空间
        myctx = ngx_palloc(r->pool, sizeof(ngx_http_lcwupstream_ctx_t));
        if (myctx == NULL)//还失败
        {
            return NGX_ERROR;//返回
        }
        //将新建的上下文与请求关联起来
        //ngx_http_set_module_ctx是一个宏定义：(r)->ctx[module.ctx_index]=c;r为ngx_http_request_t指针
        ngx_http_set_ctx(r, myctx, ngx_http_lcwupstream_module);
    }
    //对每1个要使用upstream的请求，必须调用且只能调用1次
    //ngx_http_upstream_create方法，它会初始化r->upstream成员
    if (ngx_http_upstream_create(r) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_upstream_create() failed");
        return NGX_ERROR;
    }

    //得到配置结构体ngx_http_lcwupstream_conf_t
    ngx_http_lcwupstream_conf_t  *mycf = (ngx_http_lcwupstream_conf_t  *) ngx_http_get_module_loc_conf(r, ngx_http_lcwupstream_module);
    ngx_http_upstream_t *u = r->upstream;
    //这里用配置文件中的结构体来赋给r->upstream->conf成员
    u->conf = &mycf->upstream;
    //决定转发包体时使用的缓冲区
    u->buffering = mycf->upstream.buffering;

    //以下代码开始初始化resolved结构体，用来保存上游服务器的地址
    //resolved为ngx_http_upstream_resolved_t类型的指针，用于直接指定上游服务器的地址
    u->resolved = (ngx_http_upstream_resolved_t*) ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL)//resolved结构体初始化失败
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_pcalloc resolved error. %s.", strerror(errno));
        return NGX_ERROR;
    }

    //这里的上游服务器就是s.taobao.com淘宝搜索
    static struct sockaddr_in backendSockAddr;
    //得到给定主机名的包含主机名字和地址信息的hostent结构指针  
    struct hostent *pHost = gethostbyname((char*) "s.taobao.com");
    if (pHost == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "gethostbyname fail. %s", strerror(errno));

        return NGX_ERROR;
    }

    //访问上游服务器的80端口
    backendSockAddr.sin_family = AF_INET;
    backendSockAddr.sin_port = htons((in_port_t) 80);
    //将IP转换成一个互联网标准点分格式的字符串
    char* pDmsIP = inet_ntoa(*(struct in_addr*) (pHost->h_addr_list[0]));
    //将字符串转换为32位二进制网络字节序的IPV4地址
    backendSockAddr.sin_addr.s_addr = inet_addr(pDmsIP);
    myctx->backendServer.data = (u_char*)pDmsIP;
    myctx->backendServer.len = strlen(pDmsIP);

    //将地址设置到resolved成员中
    //typedef struct{
    //....
    //ngx_uint_t naddrs;//地址个数
    //struct sockaddr *sockaddr;//上游服务器的地址
    //socklen_t socklen;//长度
    //....
    //}ngx_http_upstream_resolved_t；
    u->resolved->sockaddr = (struct sockaddr *)&backendSockAddr;
    u->resolved->socklen = sizeof(struct sockaddr_in);
    u->resolved->naddrs = 1;
    //ngx_http_upstream_t有8个回调方法
    //设置三个必须实现的回调方法
    u->create_request = lcw_upstream_create_request;
    u->process_header = lcw_process_status_line;
    u->finalize_request = lcw_upstream_finalize_request;

    //这里必须将count成员加1，告诉HTTP框架将当前请求的引用计数加1，即告诉ngx_http_lcwupstream_handler方法暂时不要
    //销毁请求，因为HTTP框架只有在引用计数为0时才正真地销毁请求
    r->main->count++;
    //启动upstream机制
    ngx_http_upstream_init(r);
    //必须返回NGX_DONE
    return NGX_DONE;//通过返回NGX_DONE告诉HTTP框架暂停执行请求的下一个阶段
}