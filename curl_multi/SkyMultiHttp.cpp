#include <assert.h>  
#include <stdio.h>  
#include <stdlib.h>  
#include "SkyMultiHttp.h"

uv_loop_t *  CSkyMultiHttp::m_loop = NULL;
CURLM *      CSkyMultiHttp::m_curl_handle = NULL;
uv_timer_t   CSkyMultiHttp::m_timeout = { 0 };
CSkyMultiHttp* CSkyMultiHttp::m_obj = NULL;
done_proc CSkyMultiHttp::m_done_proc = NULL;

CSkyMultiHttp::CSkyMultiHttp(done_proc proc)
{
  CSkyMultiHttp::m_obj = this;
  CSkyMultiHttp::m_done_proc = proc;
}


CSkyMultiHttp::~CSkyMultiHttp()
{

}

BOOL CSkyMultiHttp::Init()
{
  /*libuv 提供了一个默认的事件循环, 你可以通过 uv_default_loop 来获得该事件循环,  
  如果你的程序中只有一个事件循环, 你就应该使用 libuv 为我们提供的默认事件循环*/  
  m_loop = uv_default_loop();  
  
  
  if (curl_global_init(CURL_GLOBAL_ALL)) {  
      fprintf(stderr, "Could not init cURL\n");  
      return FALSE;  
  }  
  
  //初始化定时器  
  uv_timer_init(m_loop, &m_timeout);  
  
  m_curl_handle = curl_multi_init();  
  //调用handle_socket回调函数，传入新建的sockfd，根据传入的action状态添加到相应的事件管理器，如封装epoll的libev或libevent。  
  curl_multi_setopt(m_curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);  
  /*当使用curl_multi_add_handle(g->multi, conn->easy)添加请求时会回调start_timeout，然后调用 
  curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles)初始化请求并得到一个socket(fd)*/  
  curl_multi_setopt(m_curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);  

  return TRUE;
}

void CSkyMultiHttp::loop()
{
    //事件循环由 uv_run 函数封装, 在使用libuv编程时, 该函数通常在最后才被调用.  
    uv_run(m_loop, UV_RUN_DEFAULT);  
    curl_multi_cleanup(m_curl_handle);  
}

curl_context_t *CSkyMultiHttp::create_curl_context(curl_socket_t sockfd) {  
    curl_context_t *context;  
  
    context = (curl_context_t*) malloc(sizeof *context);  
  
    context->sockfd = sockfd;  
  
    //使用socket描述符初始化handle  
    int r = uv_poll_init_socket(m_loop, &context->poll_handle, sockfd);  
    assert(r == 0);  
    context->poll_handle.data = context;  
  
    return context;  
}  
  
void CSkyMultiHttp::curl_close_cb(uv_handle_t *handle) {  
    curl_context_t *context = (curl_context_t*) handle->data;  
    free(context);  
}  
  
void CSkyMultiHttp::destroy_curl_context(curl_context_t *context) {  
    //uv_close关闭所有的流  
    uv_close((uv_handle_t*) &context->poll_handle, curl_close_cb);  
}  
  

//检测easy handle是否发送成功，成功则把easy handle移除出multi stack  
void CSkyMultiHttp::check_multi_info(void) {  
    // char *done_url;  
    CURLMsg *message;  
    int pending;  
    CURL* handle = NULL;
    // long response_code;
  
    while ((message = curl_multi_info_read(m_curl_handle, &pending))) {  
        switch (message->msg) {  
        case CURLMSG_DONE:  
            // curl_easy_getinfo(message->easy_handle, CURLINFO_EFFECTIVE_URL,  
            //                 &done_url);  
            // curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE,
            //                    &response_code);
            // printf("%s DONE\n", done_url);  
  
            handle = message->easy_handle;

            curl_multi_remove_handle(m_curl_handle, message->easy_handle);  
            // curl_easy_cleanup(message->easy_handle);  

            m_done_proc(handle);

            break;  
  
        default:  
            fprintf(stderr, "CURLMSG default\n");  
            abort();  
        }  
    }  
}  
  
void CSkyMultiHttp::curl_perform(uv_poll_t *req, int status, int events) {  
    //停止timer，回调函数将不会再被调用  
    uv_timer_stop(&m_timeout);  
    int running_handles;  
    int flags = 0;  
    if (status < 0)                      flags = CURL_CSELECT_ERR;  
    if (!status && events & UV_READABLE) flags |= CURL_CSELECT_IN;  
    if (!status && events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;  
  
    curl_context_t *context;  
  
    context = (curl_context_t*)req;  
  
    /*当事件管理器发现socket状态改变时通过curl_multi_socket_action(g->multi, fd, action, &g->still_running) 
    通知libcurl读写数据，然后再调用sock_cb通知事件管理器，如此反复。*/  
    curl_multi_socket_action(m_curl_handle, context->sockfd, flags, &running_handles);  
    m_obj->check_multi_info();     
}  
  
void CSkyMultiHttp::on_timeout(uv_timer_t *req) {  
    int running_handles;  
    //初始化请求并得到一个socketfd  
    curl_multi_socket_action(m_curl_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles);  
    m_obj->check_multi_info();  
}  
  
void CSkyMultiHttp::start_timeout(CURLM *multi, long timeout_ms, void *userp) {  
    if (timeout_ms <= 0)  
        timeout_ms = 1; /* 0 means directly call socket_action, but we'll do it in a bit */  
    //注册自己的定时回调函数uv_timer_start  
    uv_timer_start(&m_timeout, on_timeout, timeout_ms, 0);  
}  
  
int CSkyMultiHttp::handle_socket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp) {  
    curl_context_t *curl_context = NULL;  
    if (action == CURL_POLL_IN || action == CURL_POLL_OUT) {  
        if (socketp) {  
            curl_context = (curl_context_t*) socketp;  
        }  
        else {  
            curl_context = m_obj->create_curl_context(s);  
            curl_multi_assign(m_curl_handle, s, (void *) curl_context);  
        }  
    }  
  
    switch (action) {  
        case CURL_POLL_IN:  
            //开始polling文件描述符，一旦检测到读事件，则调用curl_perform函数，参数status设置为0  
            uv_poll_start(&curl_context->poll_handle, UV_READABLE, curl_perform);  
            break;  
        case CURL_POLL_OUT:  
            uv_poll_start(&curl_context->poll_handle, UV_WRITABLE, curl_perform);  
            break;  
        case CURL_POLL_REMOVE:  
            if (socketp) {  
                //停止polling文件描述符  
                uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);  
                m_obj->destroy_curl_context((curl_context_t*) socketp);                  
                curl_multi_assign(m_curl_handle, s, NULL);  
            }  
            break;  
        default:  
            abort();  
    }  
  
    return 0;  
}  
