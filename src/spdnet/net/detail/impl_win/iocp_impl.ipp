#ifndef SPDNET_NET_IOCP_IMPL_IPP_
#define SPDNET_NET_IOCP_IMPL_IPP_

#include <spdnet/net/detail/impl_win/iocp_impl.h>
#include <memory>
#include <iostream>
#include <cassert>
#include <spdnet/base/spin_lock.h>
#include <spdnet/base/buffer_pool.h>
#include <spdnet/net/detail/impl_win/iocp_wakeup_channel.h>
#include <spdnet/net/detail/impl_win/iocp_recv_channel.h>
#include <spdnet/net/detail/impl_win/iocp_send_channel.h>
#include <spdnet/net/task_executor.h>

namespace spdnet {
    namespace net {
        namespace detail {
            iocp_impl::iocp_impl(std::shared_ptr<task_executor> task_executor,
                                 std::shared_ptr<channel_collector> channel_collector,
                                 std::function<void(sock_t)> &&socket_close_notify_cb) noexcept
                    : handle_(CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1)), wakeup_op_(handle_),
                      task_executor_(task_executor), channel_collector_(channel_collector),
                      socket_close_notify_cb_(socket_close_notify_cb) {
            }

            iocp_impl::~iocp_impl() noexcept {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }

            bool iocp_impl::on_socket_enter(socket_data::ptr data) {
                if (data->is_server_side()) {
                    if (CreateIoCompletionPort((HANDLE) data->sock_fd(), handle_, 0, 0) == 0) {
                        return false;
                    }
                }
                auto impl = shared_from_this();
                data->recv_channel_ = std::make_shared<iocp_recv_channel>(data, impl);
                data->send_channel_ = std::make_shared<iocp_send_channel>(data, impl);
                data->recv_channel_->start_recv();
                return true;
            }

            bool iocp_impl::start_accept(sock_t listen_fd, iocp_accept_channel *channel) {
                if (::CreateIoCompletionPort((HANDLE) listen_fd, handle_, 0, 0) == 0) {
                    return false;
                }
                channel->async_accept();
                return true;
            }

            void iocp_impl::wakeup() {
                wakeup_op_.wakeup();
            }

            void iocp_impl::close_socket(socket_data::ptr data) {
                if (data->has_closed_)
                    return;

                channel_collector_->put_channel(data->recv_channel_);
                channel_collector_->put_channel(data->send_channel_);

                socket_close_notify_cb_(data->sock_fd());

                data->close();
            }

            bool iocp_impl::async_connect(sock_t fd, const end_point &addr, channel *op) {
                if (::CreateIoCompletionPort((HANDLE) fd, handle_, 0, 0) == 0) {
                    return false;
                }

                if (!connect_ex_) {
                    DWORD bytes = 0;
                    GUID guid = {0x25a207b9, 0xddf3, 0x4660,
                                 {0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}};
                    void *ptr = nullptr;
                    if (::WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
                                   &guid, sizeof(guid), &ptr, sizeof(ptr), &bytes, 0, 0) != 0) {
                        // ...
                    }
                    connect_ex_ = ptr;
                }

                if (connect_ex_) {
                    union address_union {
                        struct sockaddr head;
                        struct sockaddr_in v4;
                        struct sockaddr_in6 v6;
                    } address;
                    memset(&address, 0, sizeof(address));
                    address.head.sa_family = addr.family();
                    auto ret = ::bind(fd, &address.head,
                                      addr.family() == AF_INET ? sizeof(address.v4) : sizeof(address.v6));
                    if (ret == SPDNET_SOCKET_ERROR) {
                        // ...
                        return false;
                    }
                    void *connect_ex = connect_ex_;
                    typedef BOOL(PASCAL
                    *connect_ex_fn)(SOCKET,
                    const sockaddr*, int, void *, DWORD, DWORD *, OVERLAPPED *);
                    BOOL result = ((connect_ex_fn) connect_ex)(fd,
                                                               addr.socket_addr(), addr.socket_addr_len(), 0, 0, 0,
                                                               (LPOVERLAPPED) op);
                    DWORD last_error = ::WSAGetLastError();
                    if (!result && last_error != WSA_IO_PENDING) {
                        // ...
                        return false;
                    }
                }
                return true;
            }

            void iocp_impl::post_flush(socket_data *data) {
                task_executor_->post([data, this]() {
                    if (data->is_can_write_) {
                        data->send_channel_->flush_buffer();
                    }
                }, false);
            }

            void iocp_impl::shutdown_socket(socket_data::ptr data) {
                if (data->has_closed_)
                    return;
                ::shutdown(data->sock_fd(), SD_SEND);
                data->is_can_write_ = false;
            }

            /*
            void iocp_impl::wakeup()
            {
                ::PostQueuedCompletionStatus(handle_,0,0, (LPOVERLAPPED)wakeup_op_.get());
            }
            */
            void iocp_impl::run_once(uint32_t timeout) {
                for (;;) {
                    DWORD bytes_transferred = 0;
                    ULONG_PTR completion_key = 0;
                    LPOVERLAPPED overlapped = 0;
                    ::SetLastError(0);
                    BOOL ok = ::GetQueuedCompletionStatus(handle_, &bytes_transferred, &completion_key, &overlapped,
                                                          timeout);
                    DWORD last_error = ::GetLastError();

                    if (overlapped) {
                        channel *op = static_cast<channel *>(overlapped);
                        op->do_complete((size_t) bytes_transferred,
                                        std::error_code(last_error, std::system_category()));

                    } else if (!ok) {
                        break;
                    }

                    timeout = 0;
                }

                // del_channel_list_.clear();
            }

        }
    }

}

#endif // SPDNET_NET_IOCP_IMPL_IPP_
