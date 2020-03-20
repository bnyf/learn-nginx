#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static void* ngx_http_myupstream_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_myupstream_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t myupstream_upstream_create_request(ngx_http_request_t *r);
static ngx_int_t myupstream_process_status_line(ngx_http_request_t *r);
static ngx_int_t myupstream_upstream_process_header(ngx_http_request_t *r);
static void myupstream_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc);
static char* ngx_http_myupstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_myupstream_handler(ngx_http_request_t *r);

/* 存储该模块配置项参数的数据结构 */
typedef struct {
    ngx_str_t search_engine;
    ngx_http_upstream_conf_t upstream;
} ngx_http_myupstream_conf_t;

/* commands 数组 */
static ngx_command_t ngx_http_myupstream_commands[] = {
	{
        ngx_string("search_engine"), /* 配置项名称 */                     
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,     /* 该配置项可出现在http块中的location块内 | 该配置项有1参数 */
        ngx_conf_set_str_slot,                 /* 使用Nginx预设函数对配置项进行响应 */
        NGX_HTTP_LOC_CONF_OFFSET,               /* 指明配置项的值存储的位置 */
        /* upstream.connect_timeout 在 ngx_http_mytest_conf_t 中的偏移位置 */
        offsetof(ngx_http_myupstream_conf_t, search_engine),  
        NULL                                    /* 配置项处理后的回调函数，本模块暂时用不到 */
    },
    {
		ngx_string("myupstream"),            
		NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, 
		ngx_http_myupstream,                 /* set回调函数, 当出现了myupstream配置项后，ngx_http_myupstream函数被调用*/
        0,                                   
		0,                                   /* 使用预设方式处理配置项时有用，本模块使用的是自定义模块 */
		NULL                                 
	},
	ngx_null_command                         /* commands 数组结束标志，其值为{ ngx_null_string, 0, NULL, 0, 0, NULL } */
};

/* upstream 向下游转发过滤隐藏的头部，HTTP反向代理默认的也是这些值 */
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

/* myupstream 的上下文，upstream 每次接收到一段TCP流都会回调myupstream的process_header 方法，所以需要一个上下文来保存状态 */
typedef struct
{
    /*
        typedef struct {
            ngx_uint_t           http_version;
            ngx_uint_t           code;
            ngx_uint_t           count;
            u_char              *start;
            u_char              *end;
        } ngx_http_status_t;
    */
    ngx_http_status_t status;
    ngx_str_t backendServer;
} ngx_http_myupstream_ctx_t;

/* 这些函数是用来对自定义的存储配置的结构体进行管理，本文模块暂时用不到，所以可全设为 NULL */
static ngx_http_module_t ngx_http_mymodule_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */
    NULL, /* create main configuration */
    NULL, /* init main configuration */
    NULL, /* create server configuration */
    NULL, /* merge server configuration */
    ngx_http_myupstream_create_loc_conf, /* create location configuration */
    ngx_http_myupstream_merge_loc_conf  /* merge location configuration */
};

/* mymodule 模块 */
ngx_module_t ngx_http_myupstream_module = {
	NGX_MODULE_V1,                /* 无需定义时赋值，使用预设宏来填充 */
	&ngx_http_mymodule_module_ctx,/* 指向 HTTP 模块的上下文 */
	ngx_http_myupstream_commands,   /* 指向 commands 数组 */
	NGX_HTTP_MODULE,              /* 指明本模块为 HTTP 模块 */
    NULL,                         /* init master */
    NULL,                         /* init module */
    NULL,                         /* init process */
    NULL,                         /* init thread */
    NULL,                         /* exit thread */
    NULL,                         /* exit process */
    NULL,                         /* exit master */
	NGX_MODULE_V1_PADDING         /* 预留参数，使用预设宏来填充 */
};

/* 生成存储 loc 级别的配置参数结构体 */
static void* ngx_http_myupstream_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_myupstream_conf_t *mycf;
    
    /* 为配置分配内存 */
    mycf = (ngx_http_myupstream_conf_t *)ngx_pcalloc(cf->pool, sizeof(ngx_http_myupstream_conf_t));
    if ( mycf == NULL ) {
        return NULL;
    }

    /* 此处为了方便，此处将 ngx_http_upstream_conf_t 中的配置硬编码,超时时间都设为了HTTP反向代理默认的1分钟 */
    mycf->upstream.connect_timeout = 60000; /* 单位是毫秒 */
    mycf->upstream.send_timeout = 60000;
    mycf->upstream.read_timeout = 60000;
    mycf->upstream.store_access = 0600; 

    mycf->upstream.buffering = 0; /* 使用固定大小的内存作为缓冲区来转发响应包体 */
    mycf->upstream.bufs.num = 8;
    mycf->upstream.bufs.size = ngx_pagesize;
    mycf->upstream.buffer_size = ngx_pagesize;
    mycf->upstream.busy_buffers_size = 2 * ngx_pagesize;
    mycf->upstream.temp_file_write_size = 2 * ngx_pagesize;
    mycf->upstream.max_temp_file_size = 1024 * 1024 * 1024;

    /* 此处设为NGX_CONF_UNSET_PTR，是为后面 merge 函数中调用 ngx_http_upstream_hide_headers_hash进行初始化做准备 */
    mycf->upstream.hide_headers = NGX_CONF_UNSET_PTR;
    mycf->upstream.pass_headers = NGX_CONF_UNSET_PTR;
    
    return mycf;
}

/* 
合并 loc 级别的同名配置项
参数：cf - 配置对象
     parent - 父配置块
     child - 子配置块
返回值：成功 - NGX_CONF_OK 
       失败 - NGX_CONF_ERROR
*/
static char *ngx_http_myupstream_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_myupstream_conf_t *prev = (ngx_http_myupstream_conf_t *)parent;
    ngx_http_myupstream_conf_t *conf = (ngx_http_myupstream_conf_t *)child;

    ngx_hash_init_t  hash;
    hash.max_size = 100;
    hash.bucket_size = 1024;
    hash.name = "proxy_headers_hash";
    /* 该函数用于初始化hide_headers，只用在 merge 函数中 */
    if (ngx_http_upstream_hide_headers_hash(cf, &conf->upstream, &prev->upstream, ngx_http_proxy_hide_headers, &hash)!= NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;

}

static ngx_int_t myupstream_upstream_create_request(ngx_http_request_t *r) {
    // ngx_http_myupstream_conf_t *mycf = ngx_http_get_module_ctx(r, ngx_http_myupstream_module);
    static ngx_str_t backendQueryLine = ngx_string("GET / HTTP/1.1\r\nHost: cn.bing.com\r\nConnection: close\r\n\r\n");
    // if( ngx_strncmp(mycf->search_engine.data, "bing",  mycf->search_engine.len) == 0 ) {
    //     ngx_str_t temp = ngx_string("GET /search?q=%V HTTP/1.1\r\nHost: cn.bing.com\r\nConnection: close\r\n\r\n");
    //     backendQueryLine = temp;
    // }
    // else{
    //     ngx_str_t temp = ngx_string("GET /s?wd=%V HTTP/1.1\r\nHost: www.baidu.com\r\nConnection: close\r\n\r\n");
    //     backendQueryLine = temp;
    // }
    ngx_int_t queryLineLen = backendQueryLine.len + r->args.len - 2;

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

static ngx_int_t myupstream_process_status_line(ngx_http_request_t *r) {
    size_t                 len;
    ngx_int_t              rc;
    ngx_http_upstream_t   *u;
    //上下文中才会保存多次解析http响应行的状态，首先取出请求的上下文
    ngx_http_myupstream_ctx_t* ctx = ngx_http_get_module_ctx(r, ngx_http_myupstream_module);
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

    //下一步将开始解析http头部，设置process_header回调方法为myupstream_upstream_process_header
    //之后再收到的新字符流将由myupstream_upstream_process_header解析
    u->process_header = myupstream_upstream_process_header;
    //如果本次收到的字符流除了http响应行外，还有多余的字符，
    //将由myupstream_upstream_process_header方法解析,myupstream_upstream_process_header方法在下面实现
    return myupstream_upstream_process_header(r);
}

static ngx_int_t myupstream_upstream_process_header(ngx_http_request_t *r) {
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

static void myupstream_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc) {
    //在请求结束时，会调用该方法，可以释放资源，如打开的句柄等，由于我们没有任何需要释放的资源
    //故该方法没有任何实际的工作
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,"myupstream_upstream_finalize_request");
}

static char* ngx_http_myupstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t  *clcf;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_myupstream_handler;
    return NGX_CONF_OK;
}
/******************************************************
函数名：ngx_http_myupstream_handler(ngx_http_request_t *r)
参数：ngx_http_request_t结构体
功能：ngx_http_myupstream_handler方法的实现，启动upstream
*******************************************************/
static ngx_int_t ngx_http_myupstream_handler(ngx_http_request_t *r) {
     //首先建立http上下文结构体ngx_http_myupstream_ctx_t
    //ngx_http_get_module_ctx是一个宏定义：(r)->ctx[module.ctx_index],r为ngx_http_request_t指针
    //第二个参数为HTTP模块对象
    ngx_http_myupstream_ctx_t* myctx = ngx_http_get_module_ctx(r, ngx_http_myupstream_module);
    if (myctx == NULL)//失败
    {//开辟空间
        myctx = ngx_palloc(r->pool, sizeof(ngx_http_myupstream_ctx_t));
        if (myctx == NULL)//还失败
        {
            return NGX_ERROR;//返回
        }
        //将新建的上下文与请求关联起来
        //ngx_http_set_module_ctx是一个宏定义：(r)->ctx[module.ctx_index]=c;r为ngx_http_request_t指针
        ngx_http_set_ctx(r, myctx, ngx_http_myupstream_module);
    }
    //对每1个要使用upstream的请求，必须调用且只能调用1次
    //ngx_http_upstream_create方法，它会初始化r->upstream成员
    if (ngx_http_upstream_create(r) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_upstream_create() failed");
        return NGX_ERROR;
    }

    //得到配置结构体ngx_http_myupstream_conf_t
    ngx_http_myupstream_conf_t  *mycf = (ngx_http_myupstream_conf_t  *) ngx_http_get_module_loc_conf(r, ngx_http_myupstream_module);
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


    static struct sockaddr_in backendSockAddr;
    //得到给定主机名的包含主机名字和地址信息的hostent结构指针
    struct hostent *pHost = gethostbyname((char*) "cn.bing.com");
    // if( ngx_strncmp(mycf->search_engine.data, "bing",  mycf->search_engine.len) == 0 ) {
    //     pHost = gethostbyname((char*) "cn.bing.com");
    // }
    // else{
    //     pHost = gethostbyname((char*) "www.baidu.com");
    // }
    
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
    u->create_request = myupstream_upstream_create_request;
    u->process_header = myupstream_process_status_line;
    u->finalize_request = myupstream_upstream_finalize_request;

    //这里必须将count成员加1，告诉HTTP框架将当前请求的引用计数加1，即告诉ngx_http_myupstream_handler方法暂时不要
    //销毁请求，因为HTTP框架只有在引用计数为0时才正真地销毁请求
    r->main->count++;
    //启动upstream机制
    ngx_http_upstream_init(r);
    //必须返回NGX_DONE
    return NGX_DONE;//通过返回NGX_DONE告诉HTTP框架暂停执行请求的下一个阶段
}
