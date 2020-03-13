#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t ngx_http_mymodule_handler(ngx_http_request_t *r);
static char * ngx_http_mytest(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* commands 数组 */
static ngx_command_t ngx_http_mymodule_commands[] = {
	{
		ngx_string("mymodule"),              /* 配置项名称 */
		NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, /* 该配置项可出现在http块中的location块内 | 该配置项无参数 */
		ngx_http_mymodule,                   /* set回调函数, 当出现了mymodule配置项后，ngx_http_mymodule函数被调用*/
		NGX_HTTP_LOC_CONF_OFFSET,            /* 指明配置项的值存储的位置 */
		0,                                   /* 使用预设方式处理配置项时有用，本模块使用的是自定义模块 */
		NULL                                 /* 配置项处理后的回调函数，本模块暂时用不到 */
	},
	ngx_null_command                         /* commands 数组结束标志，其值为{ ngx_null_string, 0, NULL, 0, 0, NULL } */
};

/* 这些函数是用来对自定义的存储配置的结构体进行管理，本文模块暂时用不到，所以可全设为 NULL */
static ngx_http_module_t ngx_http_mymodule_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */
    NULL, /* create main configuration */
    NULL, /* init main configuration */
    NULL, /* create server configuration */
    NULL, /* merge server configuration */
    NULL, /* create location configuration */
    NULL  /* merge location configuration */
};

/* mymodule 模块 */
ngx_module_t ngx_http_mymodule_module = {
	NGX_MODULE_V1,                /* 无需定义时赋值，使用预设宏来填充 */
	&ngx_http_mymodule_module_ctx,/* 指向 HTTP 模块的上下文 */
	ngx_http_mymodule_commands,   /* 指向 commands 数组 */
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

/* 
commands中定义的回调函数，在解析出mymodule配置项时调用该函数
参数：cf - 配置对象
     cmd - 指向自身commands结构体的指针
     conf - 指向配置对象上下文的结构体
返回值：成功 - NGX_CONF_OK 
       失败 - NGX_CONF_ERROR
*/
static char* ngx_http_mymodule(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    /* 找到 mymodule 所在配置块 */
	ngx_http_core_loc_conf_t *clcf;
	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    /* 在HTTP处理流程走到 NGX_HTTP_CONTENT_PHASE 阶段就会调用下面定义的handler函数来处理请求 */
	clcf->handler = ngx_http_mymodule_handler;

	return NGX_CONF_OK;
}

/*
该函数用于处理HTTP请求，函数原型 typedef ngx_int_t (*ngx_http_handler_pt)(ngx_http_request_t *r);
参数：r - 存储请求的所有信息，如方法、URI、头部等
返回值：HTTP响应码或者 Nginx 错误码。如果正常响应，Nginx会根据响应码构造响应包发送给用户，响应码和错误码对应的宏定义可以在官方文档中查看，如200对应的为NGX_HTTP_OK。
*/
static ngx_int_t ngx_http_mymodule_handler(ngx_http_request_t *r)
{
    /* 如果请求方法不是 GET 或 HEAD，则返回 405 状态码 */
	if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD)))
		return NGX_HTTP_NOT_ALLOWED;

    /* 丢弃请求中的包体 */
	ngx_int_t rc = ngx_http_discard_request_body(r);
	if (rc != NGX_OK)
			return rc;

    /* 构造响应包头部中的 Content-Type */
    ngx_str_t type = ngx_string("text/plain");
    /* 构造返回包体中的内容 */
	ngx_str_t response = ngx_string("Hello F5!\n");

    /* 设置响应包中的状态码，响应包头中的Content-Length（包体长度）和 Content-Type */
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = response.len;
	r->headers_out.content_type = type;

    /* 发送包响应头 */
	rc = ngx_http_send_header(r);
    /* 如果没有包体，在此处就可以返回 */
	if (rc == NGX_ERROR || rc > NGX_OK || r->header_only)
			return rc;

    /* 构造ngx_buf_t结构体，ngx_buf_t 主要是用来处理大量数据的数据结构 */
	ngx_buf_t *b = ngx_create_temp_buf(r->pool, response.len);
	if (b == NULL)
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
    /* 将“Hello F5!”复制到 ngx_buf_t 指向的内存 */
	ngx_memcpy(b->pos, response.data, response.len);
    /* b->last 必须要设置 */
	b->last = b->pos + response.len;
    /* 表明这是最后一块 buff */
	b->last_buf = 1;

    /* ngx_chain_t 是以 ngx_buf_t 为基础的链表 */
	ngx_chain_t out;
    /* 给 ngx_chain_t 的第一个元素赋值 */
	out.buf = b;
    /* 将 next 指针置位 NULL，表明只有这一个元素 */
	out.next = NULL;

    /* 调用 ngx_http_output_filter 发送包体 */
	return ngx_http_output_filter(r, &out);
}