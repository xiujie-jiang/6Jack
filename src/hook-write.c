
#define DEFINE_HOOK_GLOBALS 1
#define DONT_BYPASS_HOOKS   1

#include "common.h"
#include "filter.h"
#include "hook-write.h"

ssize_t (* __real_write)(int fd, const void *buf, size_t nbyte);

static FilterReplyResult filter_parse_reply(FilterReplyResultBase * const rb,
                                            const void * * const buf,
                                            size_t * const nbyte)
{
    msgpack_unpacked * const message = filter_receive_message(rb->filter);
    const msgpack_object_map * const map = &message->data.via.map;
    FilterReplyResult reply_result = filter_parse_common_reply_map(rb, map);

    if (rb->pre == false) {
        return reply_result;
    }
    const msgpack_object * const obj_data =
        msgpack_get_map_value_for_key(map, "data");
    if (obj_data != NULL && obj_data->type == MSGPACK_OBJECT_RAW &&
        *rb->ret > 0) {
        *buf = obj_data->via.raw.ptr;
        *nbyte = (size_t) obj_data->via.raw.size;        
        *rb->ret = (int) *nbyte;
        assert((size_t) *rb->ret == *nbyte);
    }    
    return reply_result;
}

static FilterReplyResult filter_apply(FilterReplyResultBase * const rb,
                                      const struct sockaddr_storage * const sa_local,
                                      const socklen_t sa_local_len,
                                      const struct sockaddr_storage * const sa_remote,
                                      const socklen_t sa_remote_len,
                                      const void * * const buf,
                                      size_t * const nbyte)
{
    msgpack_packer * const msgpack_packer = rb->filter->msgpack_packer;
    filter_before_apply(rb, 1U, "write",
                        sa_local, sa_local_len, sa_remote, sa_remote_len);
    msgpack_pack_mstring(msgpack_packer, "data");
    msgpack_pack_raw(msgpack_packer, *nbyte);
    msgpack_pack_raw_body(msgpack_packer, *buf, *nbyte);
    if (filter_send_message(rb->filter) != 0) {
        return FILTER_REPLY_RESULT_ERROR;
    }
    return filter_parse_reply(rb, buf, nbyte);
}

int __real_write_init(void)
{
#ifdef USE_INTERPOSERS
    __real_write = write;
#else
    if (__real_write == NULL) {
        __real_write = dlsym(RTLD_NEXT, "write");
        assert(__real_write != NULL);
    }
#endif
    return 0;
}

ssize_t INTERPOSE(write)(int fd, const void *buf, size_t nbyte)
{
    __real_write_init();
    const bool bypass_filter =
        getenv("SIXJACK_BYPASS") != NULL || is_socket(fd) == false;
    struct sockaddr_storage sa_local, *sa_local_ = &sa_local;
    struct sockaddr_storage sa_remote, *sa_remote_ = &sa_remote;
    socklen_t sa_local_len, sa_remote_len;
    get_sock_info(fd, &sa_local_, &sa_local_len, &sa_remote_, &sa_remote_len);
    int ret = 0;
    int ret_errno = 0;    
    bool bypass_call = false;
    size_t new_nbyte = nbyte;
    FilterReplyResultBase rb = {
        .pre = true, .ret = &ret, .ret_errno = &ret_errno, .fd = fd
    };
    if (bypass_filter == false && (rb.filter = filter_get()) &&
        filter_apply(&rb, sa_local_, sa_local_len,
                     sa_remote_, sa_remote_len, &buf, &new_nbyte)
        == FILTER_REPLY_BYPASS) {
        bypass_call = true;
    }
    if (bypass_call == false) {
        ssize_t ret_ = __real_write(fd, buf, new_nbyte);
        ret_errno = errno;        
        ret = (int) ret_;
        assert((ssize_t) ret_ == ret);
    }
    if (bypass_filter == false) {
        rb.pre = false;
        filter_apply(&rb, sa_local_, sa_local_len,
                     sa_remote_, sa_remote_len, &buf, &new_nbyte);
    }
    errno = ret_errno;
    
    return ret;
}
